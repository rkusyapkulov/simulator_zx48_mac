#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdatomic.h>
#include "z80.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define BORDER_X 32
#define BORDER_Y 24
#define CORE_WIDTH 256
#define CORE_HEIGHT 192

static uint8_t keyboard_matrix[8] = { 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F };

static const uint32_t palette[16] = {
    0xFF000000, 0xFFD70000, 0xFF0000D7, 0xFFD700D7, 0xFF00D700, 0xFFD7D700, 0xFF00D7D7, 0xFFD7D7D7,
    0xFF000000, 0xFFFF0000, 0xFF0000FF, 0xFFFF00FF, 0xFF00FF00, 0xFFFFFF00, 0xFF00FFFF, 0xFFFFFFFF
};

static uint8_t zx48_rom[16384];
static uint8_t zx48_ram[49152];
static uint32_t pixel_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];

static uint8_t beeper_bit = 0;
static uint8_t spec_border_color = 7;
static FILE *active_tape_file = NULL;

// Временная шкала CPU. cpu_tstates_total монотонен между сбросами;
// current_frame_tstates используется только для локального времени кадра.
#define CPU_CLOCK_HZ       3494400ULL  // 69888 T-state * 50 кадров/с (Spectrum 48K)
#define AY_CLOCK_HZ        1773400ULL  // приблизительная частота AY на Spectrum 128K
#define AUDIO_SAMPLE_RATE  44100ULL
static uint32_t current_frame_tstates = 0;
static uint64_t cpu_tstates_total = 0;
static uint64_t audio_cursor_tstates = 0;
static uint64_t ay_clock_phase = 0;
static uint64_t audio_sample_phase = 0;
static uint8_t noise_prescaler = 0;
static uint8_t envelope_prescaler = 0;

// ============================================================================
//   СТРУКТУРЫ И БУФЕР АУДИОСОБЫТИЙ ДЛЯ СИНХРОНИЗАЦИИ
// ============================================================================
typedef enum {
    EVENT_AY_REG_SELECT,
    EVENT_AY_REG_WRITE,
    EVENT_BEEPER_WRITE
} AudioEventType;

typedef struct {
    AudioEventType type;
    uint64_t tstate;
    uint8_t value;
} AudioEvent;

#define AUDIO_EVENT_CAPACITY 32768
static AudioEvent audio_event_buffer[AUDIO_EVENT_CAPACITY];
static uint32_t audio_event_head = 0;
static uint32_t audio_event_tail = 0;

void QueueAudioEvent(AudioEventType type, uint8_t value) {
    uint32_t next_head = (audio_event_head + 1) % AUDIO_EVENT_CAPACITY;
    if (next_head != audio_event_tail) {
        audio_event_buffer[audio_event_head].type = type;
        audio_event_buffer[audio_event_head].tstate = cpu_tstates_total;
        audio_event_buffer[audio_event_head].value = value;
        audio_event_head = next_head;
    }
}

// Состояние AY-чипа для рендера
struct AY_State {
    uint8_t regs[16];
    uint8_t selected_reg;
    
    int32_t tone_counters[3];
    uint8_t tone_outputs[3];
    
    int32_t noise_counter;
    uint32_t noise_shift_reg;
    uint8_t noise_output;
} ay;

struct AY_Envelope {
    uint32_t count;             // делитель периода огибающей
    int32_t step;               // 15..0
    uint8_t attack_mask;        // 0x00 или 0x0F
    uint8_t holding;
} ay_env;

// Локальная копия регистров для пошагового наката внутри рендера
static uint8_t local_ay_regs[16];
static uint8_t local_ay_selected_reg = 0;
static uint8_t local_beeper_bit = 0;

#define AUDIO_RING_SIZE 16384
static int16_t audio_ring_buffer[AUDIO_RING_SIZE];
static atomic_uint_least32_t audio_ring_head = ATOMIC_VAR_INIT(0);
static atomic_uint_least32_t audio_ring_tail = ATOMIC_VAR_INIT(0);

// Single-producer (поток эмуляции) / single-consumer (аудиоколбэк) ring buffer.
void audio_ring_push(int16_t sample) {
    uint32_t head = atomic_load_explicit(&audio_ring_head, memory_order_relaxed);
    uint32_t next_head = (head + 1u) % AUDIO_RING_SIZE;
    uint32_t tail = atomic_load_explicit(&audio_ring_tail, memory_order_acquire);
    if (next_head != tail) {
        audio_ring_buffer[head] = sample;
        atomic_store_explicit(&audio_ring_head, next_head, memory_order_release);
    }
    // При переполнении отбрасываем новый сэмпл; индексы остаются согласованными.
}

void zx48_get_audio_samples(int16_t *buffer, int sample_count) {
    if (!buffer || sample_count <= 0) return;
    for (int i = 0; i < sample_count; i++) {
        uint32_t tail = atomic_load_explicit(&audio_ring_tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&audio_ring_head, memory_order_acquire);
        if (tail != head) {
            buffer[i] = audio_ring_buffer[tail];
            atomic_store_explicit(&audio_ring_tail, (tail + 1u) % AUDIO_RING_SIZE, memory_order_release);
        } else {
            buffer[i] = 0;
        }
    }
}

uint8_t ReadByte(uint16_t address) {
    if (address < 0x4000) return zx48_rom[address];
    return zx48_ram[address - 0x4000];
}

void WriteByte(uint16_t address, uint8_t value) {
    if (address < 0x4000) return;
    zx48_ram[address - 0x4000] = value;
}

// ============================================================================
//   ОБНОВЛЕННАЯ ДЕШИФРАЦИЯ ПОРТОВ С СОХРАНЕНИЕМ ИСТОРИИ СОБЫТИЙ
// ============================================================================
void OutPort(uint16_t port, uint8_t value) {
    // Выбор активного регистра AY
    if (port == 0xFFFD) {
        ay.selected_reg = value & 0x0F;
        QueueAudioEvent(EVENT_AY_REG_SELECT, value & 0x0F);
        return;
    }
    // Запись данных в активный регистр AY
    if (port == 0xBFFD) {
        ay.regs[ay.selected_reg] = value;
        QueueAudioEvent(EVENT_AY_REG_WRITE, value);
        return;
    }
    // Системный порт ULA (бипер, бордюр)
    if ((port & 0x00FF) == 0x00FE) {
        spec_border_color = value & 0x07;
        beeper_bit = (value & 0x10) ? 1 : 0;
        QueueAudioEvent(EVENT_BEEPER_WRITE, beeper_bit);
    }
}

uint8_t InPort(uint16_t port) {
    // 1. Чтение матрицы клавиатуры: если младший байт адреса равен 0xFE
    if ((port & 0x00FF) == 0x00FE) {
        uint8_t result = 0x1F;
        uint8_t high_byte = (port >> 8) & 0xFF;
        for (int i = 0; i < 8; i++) {
            if (!(high_byte & (1 << i))) {
                result &= keyboard_matrix[i];
            }
        }
        return result | 0xE0;
    }

    // 2. Чтение текущего состояния выбранного регистра AY
    if (port == 0xBFFD) {
        // Защита I/O портов: возвращаем 0xFF (Kempston Joystick опрашивается по 0x1F, а не через AY)
        if (ay.selected_reg == 14 || ay.selected_reg == 15) {
            return 0xFF;
        }
        return ay.regs[ay.selected_reg];
    }
    
    if (port == 0xFFFD) {
        return ay.selected_reg;
    }
    
    return 0xFF; // Floating bus по умолчанию
}

// ============================================================================
//   ТОЧНЫЙ СИНХРОННЫЙ АУДИОДВИЖОК С АНАЛИЗОМ СОБЫТИЙ БУФЕРА
// ============================================================================
static void ay_clock_tick(void) {
    // Tone generators: one counter step per AY clock / 8.
    for (int chan = 0; chan < 3; chan++) {
        uint32_t period = ((uint32_t)(local_ay_regs[chan * 2 + 1] & 0x0F) << 8) |
                          local_ay_regs[chan * 2];
        if (period == 0) period = 1;
        if (++ay.tone_counters[chan] >= (int32_t)period) {
            ay.tone_counters[chan] -= (int32_t)period;
            ay.tone_outputs[chan] ^= 1;
        }
    }

    // Noise advances at AY clock / 16, i.e. once per two tone ticks.
    if (++noise_prescaler >= 2) {
        noise_prescaler = 0;
        uint32_t noise_period = local_ay_regs[6] & 0x1F;
        if (noise_period == 0) noise_period = 1;
        if (++ay.noise_counter >= (int32_t)noise_period) {
            ay.noise_counter = 0;
            uint8_t feedback = (uint8_t)((ay.noise_shift_reg ^ (ay.noise_shift_reg >> 3)) & 1u);
            ay.noise_shift_reg = (ay.noise_shift_reg >> 1) | ((uint32_t)feedback << 16);
            ay.noise_shift_reg &= 0x1FFFFu;
            ay.noise_output = (uint8_t)(ay.noise_shift_reg & 1u);
        }
    }

    // AY envelope clock is AY clock / 256: once per 32 tone ticks.
    if (++envelope_prescaler >= 32) {
        envelope_prescaler = 0;
        uint32_t env_period = ((uint32_t)local_ay_regs[12] << 8) | local_ay_regs[11];
        if (env_period == 0) env_period = 1;
        if (!ay_env.holding && ++ay_env.count >= env_period) {
            ay_env.count = 0;
            --ay_env.step;
            if (ay_env.step < 0) {
                uint8_t shape = local_ay_regs[13] & 0x0F;
                if ((shape & 0x08) == 0) {
                    // Continue=0: finish at the level selected by Attack.
                    ay_env.step = 0;
                    ay_env.holding = 1;
                } else if (shape & 0x01) {
                    // Hold=1: optionally invert the final level, then stop.
                    if (shape & 0x02) ay_env.attack_mask ^= 0x0F;
                    ay_env.step = 0;
                    ay_env.holding = 1;
                } else {
                    // Continuous shape: restart; Alternate reverses direction.
                    if (shape & 0x02) ay_env.attack_mask ^= 0x0F;
                    ay_env.step = 15;
                }
            }
        }
    }
}

static void apply_due_audio_events(void) {
    while (audio_event_tail != audio_event_head) {
        AudioEvent *evt = &audio_event_buffer[audio_event_tail];
        if (evt->tstate > audio_cursor_tstates) break;

        if (evt->type == EVENT_AY_REG_SELECT) {
            local_ay_selected_reg = evt->value & 0x0F;
        } else if (evt->type == EVENT_AY_REG_WRITE) {
            local_ay_regs[local_ay_selected_reg] = evt->value;
            if (local_ay_selected_reg == 13) {
                uint8_t shape = evt->value & 0x0F;
                ay_env.count = 0;
                ay_env.step = 15;
                ay_env.attack_mask = (shape & 0x04) ? 0x0F : 0x00;
                ay_env.holding = 0;
            }
        } else if (evt->type == EVENT_BEEPER_WRITE) {
            local_beeper_bit = evt->value ? 1 : 0;
        }
        audio_event_tail = (audio_event_tail + 1u) % AUDIO_EVENT_CAPACITY;
    }
}

static int16_t mix_audio_sample(void) {
    static const int16_t volume_table[16] = {
        0, 200, 290, 420, 610, 880, 1270, 1840,
        2660, 3850, 5570, 8060, 11660, 16860, 24400, 32767
    };
    uint8_t mixer = local_ay_regs[7];
    int32_t ay_mixed_out = 0;

    for (int chan = 0; chan < 3; chan++) {
        bool tone_disabled = (mixer & (1u << chan)) != 0;
        bool noise_disabled = (mixer & (1u << (chan + 3))) != 0;
        uint8_t chan_out = 1;
        if (!tone_disabled) chan_out &= ay.tone_outputs[chan];
        if (!noise_disabled) chan_out &= ay.noise_output;
        if (chan_out) {
            uint8_t vol = local_ay_regs[8 + chan];
            uint8_t level = (vol & 0x10)
                ? (uint8_t)((ay_env.step ^ ay_env.attack_mask) & 0x0F)
                : (uint8_t)(vol & 0x0F);
            ay_mixed_out += volume_table[level] / 3;
        }
    }

    // Сохраняем прежний уровень бипера; полноценный DC-фильтр здесь не меняем.
    int32_t sample = ay_mixed_out + (local_beeper_bit ? 4000 : 0);
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;
    return (int16_t)sample;
}

void zx48_tick_audio(uint32_t cycles) {
    if (ay.noise_shift_reg == 0) ay.noise_shift_reg = 1;
    uint64_t target_tstates = cpu_tstates_total;
    (void)cycles; // Интервал берётся из монотонной временной шкалы CPU.

    // Обрабатываем временную шкалу по T-state. Это сохраняет порядок событий AY,
    // фазу генераторов и частоту сэмплирования независимо от границ кадров/инструкций.
    while (audio_cursor_tstates < target_tstates) {
        apply_due_audio_events();

        ay_clock_phase += AY_CLOCK_HZ;
        const uint64_t ay_clock_threshold = CPU_CLOCK_HZ * 8ULL;
        while (ay_clock_phase >= ay_clock_threshold) {
            ay_clock_phase -= ay_clock_threshold;
            ay_clock_tick();
        }

        ++audio_cursor_tstates;
        audio_sample_phase += AUDIO_SAMPLE_RATE;
        if (audio_sample_phase >= CPU_CLOCK_HZ) {
            audio_sample_phase -= CPU_CLOCK_HZ;
            audio_ring_push(mix_audio_sample());
        }
    }
    // События на текущей границе времени применяются сейчас, не откладываясь до
    // следующего сэмпла/кадра.
    apply_due_audio_events();
}

void zx48_init(const char *rom_filepath) {
    memset(zx48_rom, 0, sizeof(zx48_rom));
    memset(zx48_ram, 0, sizeof(zx48_ram));
    memset(pixel_buffer, 0, sizeof(pixel_buffer));
    if (active_tape_file) { fclose(active_tape_file); active_tape_file = NULL; }
    for (int i = 0; i < 8; i++) keyboard_matrix[i] = 0x1F;
    
    FILE *f = fopen(rom_filepath, "rb");
    if (f) {
        fread(zx48_rom, 1, 16384, f);
        fclose(f);
    }
    init();
}

// ============================================================================
//   ФУНКЦИЯ ПОЛНОГО СБРОСА ЭМУЛЯТОРА (RESET)
// ============================================================================
void zx48_reset(void) {
    // 1. Очищаем оперативную память (48 КБ)
    memset(zx48_ram, 0, sizeof(zx48_ram));

    // 2. Очищаем видеобуфер экрана
    memset(pixel_buffer, 0, sizeof(pixel_buffer));

    // 3. Сбрасываем матрицу клавиатуры (все клавиши отпущены)
    for (int i = 0; i < 8; i++) {
        keyboard_matrix[i] = 0x1F;
    }
    
    // 4. Сбрасываем звуковой сопроцессор AY-3-8912 и огибающую
    memset(ay.regs, 0, sizeof(ay.regs));
    memset(local_ay_regs, 0, sizeof(local_ay_regs));
    ay.selected_reg = 0;
    local_ay_selected_reg = 0;
    
    memset(ay.tone_counters, 0, sizeof(ay.tone_counters));
    memset(ay.tone_outputs, 1, sizeof(ay.tone_outputs));
    ay.noise_counter = 0;
    ay.noise_shift_reg = 1;
    ay.noise_output = 1;
    
    ay_env.count = 0;
    ay_env.step = 0;
    ay_env.attack_mask = 0;
    ay_env.holding = 1;
    
    // 5. Сбрасываем системные переменные ULA
    beeper_bit = 0;
    local_beeper_bit = 0;
    spec_border_color = 7;
    current_frame_tstates = 0;
    cpu_tstates_total = 0;
    audio_cursor_tstates = 0;
    ay_clock_phase = 0;
    audio_sample_phase = 0;
    noise_prescaler = 0;
    envelope_prescaler = 0;

    audio_event_head = 0;
    audio_event_tail = 0;
    // Не сбрасываем индексы SPSC-буфера из потока эмуляции: tail принадлежит
    // аудиопотребителю. Уже поставленные сэмплы будут дочитаны аудиоколбэком.
    
    // 6. Если была вставлена лента, закрываем её
    if (active_tape_file) {
        fclose(active_tape_file);
        active_tape_file = NULL;
    }
    
    // 7. Вызываем аппаратный сброс процессора Z80 (PC = 0x0000, сброс флагов и прерываний)
    init();

    printf("Эмулятор успешно перезапущен (Hard Reset).\n");
}

void RenderSpectrumScreenToBuffer(void) {
    uint32_t border_rgba = palette[spec_border_color];
    
    // 1. Отрисовка верхней рамки бордюра (высота BORDER_Y)
    for (int i = 0; i < SCREEN_WIDTH * BORDER_Y; i++) {
        pixel_buffer[i] = border_rgba;
    }

    const uint8_t *screen_data = &zx48_ram[0x0000];
    const uint8_t *attr_data   = &zx48_ram[0x1800];

    // 2. Отрисовка рабочей области и боковых рамок
    for (int y = 0; y < CORE_HEIGHT; y++) {
        int dest_y = y + BORDER_Y;
        int line_start_idx = dest_y * SCREEN_WIDTH;

        // Левый бордюр для текущей строки
        for (int x = 0; x < BORDER_X; x++) {
            pixel_buffer[line_start_idx + x] = border_rgba;
        }

        // Вычисление смещений в памяти Spectrum
        int third = y >> 6;
        int row_in_cell = y & 0x07;
        int cell_row = (y >> 3) & 0x07;
        uint16_t pixel_line_offset = (third << 11) | (row_in_cell << 8) | (cell_row << 5);
        uint16_t attr_offset = (third << 8) | (cell_row << 5);

        // Центральная часть (256 пикселей = 32 байта)
        for (int x_byte = 0; x_byte < 32; x_byte++) {
            uint8_t pixels = screen_data[pixel_line_offset + x_byte];
            uint8_t attr   = attr_data[attr_offset + x_byte];
            
            uint8_t ink    = attr & 0x07;
            uint8_t paper  = (attr >> 3) & 0x07;
            uint8_t bright = (attr & 0x40) ? 8 : 0;
            
            uint32_t ink_rgba   = palette[ink + bright];
            uint32_t paper_rgba = palette[paper + bright];
            
            int dest_pixel_idx = line_start_idx + BORDER_X + (x_byte * 8);

            // Развертывание битовой маски байта
            pixel_buffer[dest_pixel_idx + 0] = (pixels & 0x80) ? ink_rgba : paper_rgba;
            pixel_buffer[dest_pixel_idx + 1] = (pixels & 0x40) ? ink_rgba : paper_rgba;
            pixel_buffer[dest_pixel_idx + 2] = (pixels & 0x20) ? ink_rgba : paper_rgba;
            pixel_buffer[dest_pixel_idx + 3] = (pixels & 0x10) ? ink_rgba : paper_rgba;
            pixel_buffer[dest_pixel_idx + 4] = (pixels & 0x08) ? ink_rgba : paper_rgba;
            pixel_buffer[dest_pixel_idx + 5] = (pixels & 0x04) ? ink_rgba : paper_rgba;
            pixel_buffer[dest_pixel_idx + 6] = (pixels & 0x02) ? ink_rgba : paper_rgba;
            pixel_buffer[dest_pixel_idx + 7] = (pixels & 0x01) ? ink_rgba : paper_rgba;
        }

        // Правый бордюр для текущей строки
        int right_border_start = line_start_idx + BORDER_X + CORE_WIDTH;
        for (int x = 0; x < BORDER_X; x++) {
            pixel_buffer[right_border_start + x] = border_rgba;
        }
    }

    // 3. Отрисовка нижней рамки бордюра
    int bottom_border_start = (BORDER_Y + CORE_HEIGHT) * SCREEN_WIDTH;
    for (int i = bottom_border_start; i < SCREEN_WIDTH * SCREEN_HEIGHT; i++) {
        pixel_buffer[i] = border_rgba;
    }
}

// Внутренний обработчик перехвата ПЗУ Spectrum (Вызывается прямо внутри цикла выполнения инструкций)
static void HandleRomTapeHook(void) {
    if (!active_tape_file) return;

    // Считываем точные параметры запроса из регистровой структуры Z80 ядра
    // IX = адрес назначения, DE = длина запрашиваемого блока, A = ожидаемый флаг типа
    uint16_t dest_address = cpu.IX;
    uint16_t expected_len = ((uint16_t)cpu.D << 8) | cpu.E;
    uint8_t expected_flag = cpu.A;
    uint16_t block_len = 0;
    
    // Считываем заголовок очередного блока из TAP-файла
    if (fread(&block_len, 2, 1, active_tape_file) == 1) {
        uint8_t *buffer = malloc(block_len);
        if (fread(buffer, 1, block_len, active_tape_file) == block_len) {
            uint8_t current_flag = buffer[0];

            // Загрузка срабатывает только если флаг совпадает с ожидаемым системным (0x00 или 0xFF)
            if (current_flag == expected_flag) {
                uint16_t actual_data_len = block_len - 2; // Минус байт флага и байт контрольной суммы
                if (actual_data_len > expected_len) actual_data_len = expected_len;

                // Побайтово инжектируем блок данных прямо в ОЗУ эмулятора
                if (dest_address >= 0x4000) {
                    for (uint16_t i = 0; i < actual_data_len; i++) {
                        WriteByte(dest_address + i, buffer[1 + i]);
                    }
                    // Обновляем регистры HL в соответствии со стандартами выхода из подпрограммы ПЗУ
                    uint16_t target = dest_address + actual_data_len;
                    cpu.H = target >> 8;
                    cpu.L = target & 0xFF;
                }

                // Корректное перестроение флагов Z80 ядра для выхода из процедуры загрузки ПЗУ
                cpu.F = 0x01;    // Сигнал для ОС: Блок прочитан успешно (Carry Flag = 1)
                cpu.A = current_flag;
                cpu.D = 0;
                cpu.E = 0;
                cpu.PC = 0x056B; // Принудительный скачок на адрес успешного завершения LOAD в Spectrum ROM
            } else {
                // Если блок в TAP-файле не совпал с ожидаемым (например, пропустили заголовок),
                // возвращаем указатель файла назад, чтобы не пропустить нужные данные
                fseek(active_tape_file, -(block_len + 2), SEEK_CUR);
            }
        }
        free(buffer);
    } else {
        // Если лента докрутилась до конца, закрываем файл магнитофона
        fclose(active_tape_file);
        active_tape_file = NULL;
    }
}

void zx48_run_frame(void) {
    // ВАЖНО: Очередь аудиособытий больше НЕ сбрасывается здесь в 0.
    // Это предотвращает потерю отложенных звуков между кадрами.
    current_frame_tstates = 0;

    int cycles_executed = 0;
    while (cycles_executed < 69888) {
        int cycles = step();
        cycles_executed += cycles;
        current_frame_tstates += cycles;
        cpu_tstates_total += (uint32_t)cycles;

        zx48_tick_audio((uint32_t)cycles);

        // ХУК ПЗУ: Если процессор заходит на адрес начала подпрограммы чтения блока с ленты
        if (cpu.PC == 0x0556) {
            HandleRomTapeHook();
        }
    }
    z80_interrupt_now(0xFF);
    RenderSpectrumScreenToBuffer();
}

const uint32_t* zx48_get_pixel_buffer(void) {
    return pixel_buffer;
}

void zx48_press_key(int row, int bit, int isPressed) {
    if (row < 0 || row > 7 || bit < 0 || bit > 4) return;
    if (isPressed) keyboard_matrix[row] &= ~(1 << bit);
    else keyboard_matrix[row] |= (1 << bit);
}

// Интерфейс подключения файла ленты из Objective-C
int zx48_load_tape(const char *filepath) {
    if (active_tape_file) {
        fclose(active_tape_file);
        active_tape_file = NULL;
    }
    active_tape_file = fopen(filepath, "rb");
    return (active_tape_file != NULL) ? 1 : 0;
}

// ============================================================================
//   ДОБАВЛЕННЫЙ МОДУЛЬ РАСПАКОВКИ И ЗАГРУЗКИ СНАПШОТОВ СТАНДАРТА .Z80
// ============================================================================
bool zx48_load_z80(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;

    uint8_t header[30];
    if (fread(header, 1, 30, f) != 30) { fclose(f); return false; }

    // Чтение базовых регистров Z80 (v1)
    cpu.A = header[0];  cpu.F = header[1];
    cpu.C = header[2];  cpu.B = header[3];
    cpu.L = header[4];  cpu.H = header[5];
    
    uint16_t pc = header[6] | (header[7] << 8);
    cpu.SP = header[8] | (header[9] << 8);
    cpu.I = header[10];
    cpu.RR = header[11];

    uint8_t indicator = header[12];
    if (indicator == 255) indicator = 1; // Корректировка флага сжатия данных v1
    spec_border_color = (indicator >> 1) & 0x07;

    cpu.E = header[13]; cpu.D = header[14];
    cpu.C_alt = header[15]; cpu.B_alt = header[16];
    cpu.E_alt = header[17]; cpu.D_alt = header[18];
    cpu.L_alt = header[19]; cpu.H_alt = header[20];
    cpu.A_alt = header[21]; cpu.F_alt = header[22];
    cpu.IY = header[23] | (header[24] << 8);
    cpu.IX = header[25] | (header[26] << 8);
    cpu.IFF1 = header[27] != 0; cpu.IFF2 = header[28] != 0;
    cpu.IM = header[29] & 3;

    cpu.halted = false; cpu.int_pending = false; cpu.nmi_pending = false;

    bool version_1 = (pc != 0);
    long compressed_data_start = 30;

    if (!version_1) {
        // Снапшот v2 или v3
        uint16_t ext_header_len = fgetc(f) | (fgetc(f) << 8);
        uint8_t *ext_header = malloc(ext_header_len);
        fread(ext_header, 1, ext_header_len, f);

        pc = ext_header[0] | (ext_header[1] << 8);
        compressed_data_start = 32 + ext_header_len;
        free(ext_header);
    }
    cpu.PC = pc;

    fseek(f, compressed_data_start, SEEK_SET);

    // Чтение и декомпрессия упакованной памяти Z80 (алгоритм RLE)
    if (version_1) {
        uint8_t ram_buffer[49152];
        size_t ram_ptr = 0;
        int c1, c2;

        while ((c1 = fgetc(f)) != EOF && ram_ptr < 49152) {
            if (c1 == 0xED) {
                c2 = fgetc(f);
                if (c2 == 0xED) {
                    int count = fgetc(f);
                    int val = fgetc(f);
                    for (int k = 0; k < count && ram_ptr < 49152; k++) {
                        ram_buffer[ram_ptr++] = val;
                    }
                } else {
                    if (ram_ptr < 49152) ram_buffer[ram_ptr++] = 0xED;
                    if (c2 != EOF && ram_ptr < 49152) ram_buffer[ram_ptr++] = c2;
                }
            } else {
                ram_buffer[ram_ptr++] = c1;
            }
        }
        memcpy(zx48_ram, ram_buffer, 49152);
    } else {
        // Декомпрессия постраничных блоков памяти v2/v3 (для 48K Spectrum)
        uint16_t block_size;
        while (fread(&block_size, 2, 1, f) == 1) {
            uint8_t page_num = fgetc(f);
            uint8_t *target_ram_bank = NULL;

            // Маппинг страниц памяти формата .z80 v2 на линейную RAM 48K эмулятора
            if (page_num == 4)      target_ram_bank = &zx48_ram[0x4000 - 0x4000];
            else if (page_num == 5) target_ram_bank = &zx48_ram[0x8000 - 0x4000];
            else if (page_num == 8) target_ram_bank = &zx48_ram[0xC000 - 0x4000];

            if (block_size == 0xFFFF) {
                if (target_ram_bank) fread(target_ram_bank, 1, 16384, f);
                else fseek(f, 16384, SEEK_CUR);
            } else {
                uint8_t *comp_buf = malloc(block_size);
                fread(comp_buf, 1, block_size, f);
                
                if (target_ram_bank) {
                    size_t out_idx = 0;
                    for (size_t in_idx = 0; in_idx < block_size; ) {
                        if (in_idx <= block_size - 4 && comp_buf[in_idx] == 0x00 && comp_buf[in_idx+1] == 0xED && comp_buf[in_idx+2] == 0xED && comp_buf[in_idx+3] == 0x00) {
                            break;
                        }
                        if (in_idx <= block_size - 4 && comp_buf[in_idx] == 0xED && comp_buf[in_idx+1] == 0xED) {
                            int count = comp_buf[in_idx+2];
                            uint8_t value = comp_buf[in_idx+3];
                            for (int k = 0; k < count && out_idx < 16384; k++) target_ram_bank[out_idx++] = value;
                            in_idx += 4;
                        } else {
                            if (out_idx < 16384) target_ram_bank[out_idx++] = comp_buf[in_idx];
                            in_idx++;
                        }
                    }
                }
                free(comp_buf);
            }
        }
    }
    fclose(f);
    return true;
}

#pragma pack(push, 1)
typedef struct {
    uint8_t i;
    uint16_t hl_prime, de_prime, bc_prime, af_prime;
    uint16_t hl, de, bc, iy, ix;
    uint8_t iff1_2;
    uint8_t r;
    uint16_t af, sp;
    uint8_t im;
    uint8_t border;
} SNA_Header;
#pragma pack(pop)

// --- ЗАГРУЗКА СНАПШОТА .SNA ---
bool zx48_load_sna(const char *filepath) {
    FILE *f = fopen(filepath, "rb");
    if (!f) return false;

    SNA_Header header;
    if (fread(&header, 1, sizeof(SNA_Header), f) != sizeof(SNA_Header)) {
        fclose(f);
        return false;
    }

    // 1. Восстановление регистров в оригинальную структуру `cpu`
    cpu.I = header.i;
    
    // Альтернативный набор регистров
    cpu.L_alt = header.hl_prime & 0xFF;
    cpu.H_alt = (header.hl_prime >> 8) & 0xFF;
    cpu.E_alt = header.de_prime & 0xFF;
    cpu.D_alt = (header.de_prime >> 8) & 0xFF;
    cpu.C_alt = header.bc_prime & 0xFF;
    cpu.B_alt = (header.bc_prime >> 8) & 0xFF;
    cpu.F_alt = header.af_prime & 0xFF;
    cpu.A_alt = (header.af_prime >> 8) & 0xFF;
    
    // Основной набор регистров
    cpu.L = header.hl & 0xFF;
    cpu.H = (header.hl >> 8) & 0xFF;
    cpu.E = header.de & 0xFF;
    cpu.D = (header.de >> 8) & 0xFF;
    cpu.C = header.bc & 0xFF;
    cpu.B = (header.bc >> 8) & 0xFF;
    
    // Индексные регистры и указатель стека
    cpu.IY = header.iy;
    cpu.IX = header.ix;
    cpu.SP = header.sp;
    
    // Флаги прерываний и системные регистры
    cpu.IFF1 = (header.iff1_2 & 0x04) != 0;
    cpu.IFF2 = cpu.IFF1;
    cpu.RR = header.r;
    cpu.F = header.af & 0xFF;
    cpu.A = (header.af >> 8) & 0xFF;
    
    // Режим прерываний (IM 0, IM 1, IM 2)
    cpu.IM = header.im & 3;
    if (cpu.IM > 2) cpu.IM = 2;
    
    spec_border_color = header.border & 0x07;

    // 2. Чтение 48 КБ оперативной памяти Spectrum прямо в буфер RAM
    if (fread(zx48_ram, 1, 49152, f) != 49152) {
        fclose(f);
        return false;
    }
    fclose(f);

    // 3. Специфика архитектуры .SNA 48K:
    // Адрес возврата (PC) принудительно заталкивается в стек при сохранении.
    // Извлекаем его из вершины текущего восстановленного стека SP.
    if (cpu.SP < 0x4000) return false; // Защита от указателя стека в область ПЗУ
    
    uint16_t pc_low = ReadByte(cpu.SP);
    uint16_t pc_high = ReadByte(cpu.SP + 1);
    cpu.PC = (pc_high << 8) | pc_low;
    
    // Корректируем стек (делаем POP PC)
    cpu.SP += 2;

    // Сбрасываем внутренние флаги состояния процессора ядра
    cpu.halted = false;
    cpu.int_pending = false;
    cpu.nmi_pending = false;

    return true;
}
