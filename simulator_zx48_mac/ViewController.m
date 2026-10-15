//
//  ViewController.m
//  simulator_zx48_mac
//
//  Created by user on 09.10.26.
//

#import "ViewController.h"
#import <Carbon/Carbon.h>
#import <AudioToolbox/AudioToolbox.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

// Размеры оригинального экрана ZX Spectrum (без border)
#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 1024

// Ссылки на внешние функции ядра C
extern void zx48_init(const char *rom_filepath);
extern void zx48_run_frame(void);
extern const uint32_t* zx48_get_pixel_buffer(void);
extern void zx48_press_key(int row, int bit, BOOL isPressed);
extern BOOL zx48_load_tape(const char *filepath);
extern BOOL zx48_load_z80(const char *filepath);
extern BOOL zx48_load_sna(const char *filepath);
extern void zx48_reset(void);
extern void zx48_get_audio_samples(int16_t *buffer, int sample_count);

@interface ViewController () {
    AudioQueueRef audioQueue;
    AudioQueueBufferRef audioBuffers[2];
}
@end

@implementation ViewController

// callback функция для AudioQueue (вызывается системой, когда ей нужны новые аудио-данные)
static void AudioQueueCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer) {
    int sampleCount = inBuffer->mAudioDataBytesCapacity / sizeof(int16_t);
    zx48_get_audio_samples((int16_t *)inBuffer->mAudioData, sampleCount);
    inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, NULL);
}

- (void)viewDidLoad {
    [super viewDidLoad];
    
    // Настройка слоя для рендеринга
    self.view.wantsLayer = YES;
    self.emulationLayer = [CALayer layer];
    self.emulationLayer.frame = self.view.bounds;
    self.emulationLayer.contentsGravity = kCAGravityResizeAspect;
    
    // Отключаем интерполяцию для четких пикселей (Pixel Art)
    self.emulationLayer.magnificationFilter = kCAFilterNearest;
    [self.view.layer addSublayer:self.emulationLayer];
    
    // Динамически ищем файл "48.rom" внутри ресурсов нашего приложения (App Bundle)
    NSString *romPath = [[NSBundle mainBundle] pathForResource:@"48" ofType:@"rom"];
    
    if (!romPath) {
        // Выводим понятную ошибку вместо жесткого падения
        NSAlert *alert = [[NSAlert alloc] init];
        [alert setMessageText:@"Критическая ошибка запуска"];
        [alert setInformativeText:@"Файл '48.rom' не найден в ресурсах приложения.\nПожалуйста, добавьте его в проект Xcode в секцию 'Copy Bundle Resources'."];
        [alert setAlertStyle:NSAlertStyleCritical];
        [alert runModal];
        
        // Безопасное асинхронное закрытие приложения без крэша AppKit
        dispatch_async(dispatch_get_main_queue(), ^{
            [NSApp terminate:nil];
        });
        return;
    }
    
    // Инициализация C-ядра
    zx48_init([romPath UTF8String]);
    [self initAudioEngine];
    
    // Запуск цикла 50 Гц
    [self startEmulation];
}

- (void)initAudioEngine {
    AudioStreamBasicDescription format;
    format.mSampleRate = AUDIO_SAMPLE_RATE;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    format.mBytesPerPacket = 2;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = 2;
    format.mChannelsPerFrame = 1; // Моно
    format.mBitsPerChannel = 16;
    format.mReserved = 0;
    
    AudioQueueNewOutput(&format, AudioQueueCallback, (__bridge void *)(self), NULL, NULL, 0, &audioQueue);
    
    for (int i = 0; i < 2; i++) {
        AudioQueueAllocateBuffer(audioQueue, AUDIO_BUFFER_SIZE * sizeof(int16_t), &audioBuffers[i]);
        AudioQueueCallback((__bridge void *)(self), audioQueue, audioBuffers[i]);
    }
    AudioQueueStart(audioQueue, NULL);
}

- (void)dealloc {
    AudioQueueStop(audioQueue, YES);
    AudioQueueDispose(audioQueue, YES);
    [self stopEmulation];
}

- (void)viewDidLayout {
    [super viewDidLayout];
    self.emulationLayer.frame = self.view.bounds;
}

- (void)startEmulation {
    [self stopEmulation];
    // Интервал ~0.02 секунды равен 50 Гц
    self.emulationTimer = [NSTimer scheduledTimerWithTimeInterval:0.02
                                                           target:self
                                                         selector:@selector(emulationLoop)
                                                         userInfo:nil
                                                          repeats:YES];
    [[NSRunLoop currentRunLoop] addTimer:self.emulationTimer forMode:NSRunLoopCommonModes];
}

- (void)stopEmulation {
    if (self.emulationTimer) {
        [self.emulationTimer invalidate];
        self.emulationTimer = nil;
    }
}

- (void)emulationLoop {
    // 1. Выполняем симуляцию одного кадра (69888 тактов CPU)
    zx48_run_frame();
    
    // 2. Получаем RGBA буфер экрана и обновляем UI
    const uint32_t *buffer = zx48_get_pixel_buffer();
    [self updateEmulationViewWithPixelBuffer:buffer];
}

- (void)updateEmulationViewWithPixelBuffer:(const uint32_t *)pixelBuffer {
    if (!pixelBuffer) return;

    size_t bufferSize = SCREEN_WIDTH * SCREEN_HEIGHT * 4;
    
    // Создаем копию данных для безопасной передачи CoreGraphics
    CFDataRef dataRef = CFDataCreate(kCFAllocatorDefault, (const UInt8 *)pixelBuffer, bufferSize);
    if (!dataRef) return;

    CGDataProviderRef provider = CGDataProviderCreateWithCFData(dataRef);
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    
    CGImageRef imageRef = CGImageCreate(
        SCREEN_WIDTH, SCREEN_HEIGHT,
        8,                        // bits per component
        32,                       // bits per pixel
        SCREEN_WIDTH * 4,         // bytes per row
        colorSpace,
        // Соответствует формату 0xRRGGBBAA в памяти на Mac
        kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big,
        provider, NULL, NO, kCGRenderingIntentDefault
    );
    
    // Обновляем содержимое слоя в основном потоке с правильным ARC-модификатором
    if (imageRef) {
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        self.emulationLayer.contents = (__bridge id _Nullable)(imageRef);
        [CATransaction commit];
        CGImageRelease(imageRef);
    }
    
    // Освобождаем ресурсы CoreFoundation / CoreGraphics
    CGColorSpaceRelease(colorSpace);
    CGDataProviderRelease(provider);
    CFRelease(dataRef);
}

// Вспомогательный метод для маппинга клавиш macOS на матрицу ZX Spectrum
- (void)handleKeyEvent:(NSEvent *)event isPressed:(BOOL)isPressed {
    unsigned short keyCode = [event keyCode];
    
    // РАСШИРЕННАЯ ОБРАБОТКА BACKSPACE
    if (keyCode == kVK_Delete) { // Клавиша Backspace на клавиатурах Mac
        // Эмулируем Caps Shift (Строка 0, Бит 0) + Цифра '0' (Строка 4, Бит 0)
        zx48_press_key(0, 0, isPressed);
        zx48_press_key(4, 0, isPressed);
        return;
    }
    
    switch (keyCode) {
        // Строка 0 (0XFEFE): Caps, Z, X, C, V
        case kVK_Shift:       zx48_press_key(0, 0, isPressed); break;
        case kVK_ANSI_Z:      zx48_press_key(0, 1, isPressed); break;
        case kVK_ANSI_X:      zx48_press_key(0, 2, isPressed); break;
        case kVK_ANSI_C:      zx48_press_key(0, 3, isPressed); break;
        case kVK_ANSI_V:      zx48_press_key(0, 4, isPressed); break;
            
        // Строка 1 (0XFDFE): A, S, D, F, G
        case kVK_ANSI_A:      zx48_press_key(1, 0, isPressed); break;
        case kVK_ANSI_S:      zx48_press_key(1, 1, isPressed); break;
        case kVK_ANSI_D:      zx48_press_key(1, 2, isPressed); break;
        case kVK_ANSI_F:      zx48_press_key(1, 3, isPressed); break;
        case kVK_ANSI_G:      zx48_press_key(1, 4, isPressed); break;
            
        // Строка 2 (0XFBFE): Q, W, E, R, T
        case kVK_ANSI_Q:      zx48_press_key(2, 0, isPressed); break;
        case kVK_ANSI_W:      zx48_press_key(2, 1, isPressed); break;
        case kVK_ANSI_E:      zx48_press_key(2, 2, isPressed); break;
        case kVK_ANSI_R:      zx48_press_key(2, 3, isPressed); break;
        case kVK_ANSI_T:      zx48_press_key(2, 4, isPressed); break;
            
        // Строка 3 (0XF7FE): 1, 2, 3, 4, 5
        case kVK_ANSI_1:      zx48_press_key(3, 0, isPressed); break;
        case kVK_ANSI_2:      zx48_press_key(3, 1, isPressed); break;
        case kVK_ANSI_3:      zx48_press_key(3, 2, isPressed); break;
        case kVK_ANSI_4:      zx48_press_key(3, 3, isPressed); break;
        case kVK_ANSI_5:      zx48_press_key(3, 4, isPressed); break;
            
        // Строка 4 (0XEFFE): 0, 9, 8, 7, 6
        case kVK_ANSI_0:      zx48_press_key(4, 0, isPressed); break;
        case kVK_ANSI_9:      zx48_press_key(4, 1, isPressed); break;
        case kVK_ANSI_8:      zx48_press_key(4, 2, isPressed); break;
        case kVK_ANSI_7:      zx48_press_key(4, 3, isPressed); break;
        case kVK_ANSI_6:      zx48_press_key(4, 4, isPressed); break;
            
        // Строка 5 (0XDFFE): P, O, I, U, Y
        case kVK_ANSI_P:      zx48_press_key(5, 0, isPressed); break;
        case kVK_ANSI_O:      zx48_press_key(5, 1, isPressed); break;
        case kVK_ANSI_I:      zx48_press_key(5, 2, isPressed); break;
        case kVK_ANSI_U:      zx48_press_key(5, 3, isPressed); break;
        case kVK_ANSI_Y:      zx48_press_key(5, 4, isPressed); break;
            
        // Строка 6 (0XBFFE): Enter, L, K, J, H
        case kVK_Return:      zx48_press_key(6, 0, isPressed); break;
        case kVK_ANSI_L:      zx48_press_key(6, 1, isPressed); break;
        case kVK_ANSI_K:      zx48_press_key(6, 2, isPressed); break;
        case kVK_ANSI_J:      zx48_press_key(6, 3, isPressed); break;
        case kVK_ANSI_H:      zx48_press_key(6, 4, isPressed); break;
            
        // Строка 7 (0X7FFE): Space, SymShift, M, N, B
        case kVK_Space:       zx48_press_key(7, 0, isPressed); break;
        case kVK_Option:      zx48_press_key(7, 1, isPressed); break;
        case kVK_ANSI_M:      zx48_press_key(7, 2, isPressed); break;
        case kVK_ANSI_N:      zx48_press_key(7, 3, isPressed); break;
        case kVK_ANSI_B:      zx48_press_key(7, 4, isPressed); break;

        // стрелки
        case kVK_LeftArrow:  zx48_press_key(0, 0, isPressed); zx48_press_key(3, 4, isPressed); break; // CS+5
        case kVK_DownArrow:  zx48_press_key(0, 0, isPressed); zx48_press_key(4, 4, isPressed); break; // CS+6
        case kVK_UpArrow:    zx48_press_key(0, 0, isPressed); zx48_press_key(4, 3, isPressed); break; // CS+7
        case kVK_RightArrow: zx48_press_key(0, 0, isPressed); zx48_press_key(4, 2, isPressed); break; // CS+8            

        default: break;
    }
}

- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)keyDown:(NSEvent *)event {
    [self handleKeyEvent:event isPressed:YES];
}

- (void)keyUp:(NSEvent *)event {
    [self handleKeyEvent:event isPressed:NO];
}

- (void)flagsChanged:(NSEvent *)event {
    // Обработка модификаторов (Shift, Option/Alt)
    BOOL isShift = ([event modifierFlags] & NSEventModifierFlagShift) != 0;
    BOOL isOption = ([event modifierFlags] & NSEventModifierFlagOption) != 0;
    
    zx48_press_key(0, 0, isShift);   // Caps Shift
    zx48_press_key(7, 1, isOption);  // Symbol Shift
}

- (void)viewDidAppear {
    [super viewDidAppear];
    [self setupMenu];
    [self.view.window makeFirstResponder:self];
}

- (void)setupMenu {
    NSMenu *mainMenu = [NSApp mainMenu];
    NSMenuItem *fileMenuItem = [mainMenu itemWithTitle:@"File"];
    if (!fileMenuItem) {
        fileMenuItem = [[NSMenuItem alloc] initWithTitle:@"File" action:nil keyEquivalent:@""];
        [mainMenu addItem:fileMenuItem];
    }
    NSMenu *fileMenu = [fileMenuItem submenu] ? [fileMenuItem submenu] : [[NSMenu alloc] initWithTitle:@"File"];
    [fileMenuItem setSubmenu:fileMenu];
    
    [fileMenu addItem:[NSMenuItem separatorItem]];
    
    NSMenuItem *openItem = [[NSMenuItem alloc] initWithTitle:@"Open Tape (.tap)..."
                                                      action:@selector(openTapeDialog:)
                                               keyEquivalent:@"t"];
    [openItem setTarget:self];
    [fileMenu addItem:openItem];
    
    // Добавление пункта меню для загрузки формата .Z80
    NSMenuItem *loadZ80Item = [[NSMenuItem alloc] initWithTitle:@"Open Snapshot (.z80)..." action:@selector(openZ80Dialog:) keyEquivalent:@"z"];
    loadZ80Item.target = self;
    [fileMenu addItem:loadZ80Item];
    
    NSMenuItem *loadSnaItem = [[NSMenuItem alloc] initWithTitle:@"Open Snapshot (.sna)..." action:@selector(openSnapshotDialog:) keyEquivalent:@"l"];
    loadSnaItem.target = self;
    [fileMenu addItem:loadSnaItem];
    
    [fileMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *resetItem = [[NSMenuItem alloc] initWithTitle:@"Hard Reset"
                                                       action:@selector(triggerReset:)
                                                keyEquivalent:@"r"]; // Горячие клавиши Cmd + R
    [resetItem setTarget:self];
    [fileMenu addItem:resetItem];
}

- (void)openTapeDialog:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.allowedContentTypes = @[
        [UTType typeWithFilenameExtension:@"tap"]
    ];
    
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse returnCode) {
        if (returnCode == NSModalResponseOK && panel.URL) {
            const char *cPath = [panel.URL.path UTF8String];
            
            // Приостанавливаем эмуляцию на время загрузки
            [self stopEmulation];
            
            if (zx48_load_tape(cPath)) {
                NSLog(@"Tape successfully loaded into emulator memory");
            } else {
                NSLog(@"Failed to load tape file");
            }
            
            // Возобновляем цикл 50 Гц
            [self startEmulation];
        }
    }];
}

- (void)openZ80Dialog:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseFiles = YES;
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = NO;
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"z80"]];
    
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse returnCode) {
        if (returnCode == NSModalResponseOK && panel.URL) {
            [self stopEmulation];
            if (zx48_load_z80([panel.URL.path UTF8String])) {
                NSLog(@"Snapshot .z80 successfully uncompressed and running");
            } else {
                NSLog(@"Failed to parse .z80 snapshot file structure");
            }
            [self startEmulation];
        }
    }];
}

- (void)openSnapshotDialog:(id)sender {
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"sna"]];
    [panel beginSheetModalForWindow:self.view.window completionHandler:^(NSModalResponse returnCode) {
        if (returnCode == NSModalResponseOK && panel.URL) {
            [self stopEmulation];
            zx48_load_sna([panel.URL.path UTF8String]);
            [self startEmulation];
        }
    }];
}

- (void)triggerReset:(id)sender {
    // 1. Временно останавливаем Cocoa-таймер, чтобы избежать состояния гонки
    [self stopEmulation];
    
    // 2. Вызываем Си-функцию сброса ядра
    zx48_reset();
    
    // 3. Перезапускаем таймер эмуляции с чистого листа
    [self startEmulation];
    
    NSLog(@"Emulator reset triggered from application menu.");
}

@end

