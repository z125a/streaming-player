#import "SPPlayer.h"
#import <AVFoundation/AVFoundation.h>
#import <OpenGLES/EAGL.h>
#import <OpenGLES/ES2/gl.h>
#include <memory>
#include <thread>

extern "C" {
#include <libavformat/avformat.h>
}

#include "common/log.h"
#include "common/packet_queue.h"
#include "common/frame_queue.h"
#include "demuxer/demuxer.h"
#include "decoder/decoder_factory.h"
#include "render/gl_render.h"

// --- GLKView-based video render view ---
@interface SPPlayerView : UIView
@property (nonatomic, strong) EAGLContext *glContext;
@property (nonatomic, assign) GLuint framebuffer;
@property (nonatomic, assign) GLuint colorRenderbuffer;
@property (nonatomic, assign) CGSize renderSize;
@end

@implementation SPPlayerView

+ (Class)layerClass {
    return [CAEAGLLayer class];
}

- (instancetype)initWithFrame:(CGRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        CAEAGLLayer *eaglLayer = (CAEAGLLayer *)self.layer;
        eaglLayer.opaque = YES;
        eaglLayer.drawableProperties = @{
            kEAGLDrawablePropertyRetainedBacking: @NO,
            kEAGLDrawablePropertyColorFormat: kEAGLColorFormatRGBA8
        };

        _glContext = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
        [EAGLContext setCurrentContext:_glContext];

        glGenFramebuffers(1, &_framebuffer);
        glGenRenderbuffers(1, &_colorRenderbuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, _framebuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, _colorRenderbuffer);
        [_glContext renderbufferStorage:GL_RENDERBUFFER fromDrawable:eaglLayer];
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                  GL_RENDERBUFFER, _colorRenderbuffer);

        GLint width, height;
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
        _renderSize = CGSizeMake(width, height);
        glViewport(0, 0, width, height);
    }
    return self;
}

- (void)present {
    [_glContext presentRenderbuffer:GL_RENDERBUFFER];
}

- (void)dealloc {
    if (_framebuffer) glDeleteFramebuffers(1, &_framebuffer);
    if (_colorRenderbuffer) glDeleteRenderbuffers(1, &_colorRenderbuffer);
}

@end

// --- Player implementation ---
@interface SPPlayer () {
    sp::PacketQueue _videoPktQueue;
    sp::PacketQueue _audioPktQueue;
    sp::FrameQueue _videoFrameQueue;
    sp::FrameQueue _audioFrameQueue;

    std::unique_ptr<sp::Demuxer> _demuxer;
    std::unique_ptr<sp::IDecoder> _videoDecoder;
    std::unique_ptr<sp::IDecoder> _audioDecoder;
    std::unique_ptr<sp::GLVideoRender> _glRender;

    std::thread _renderThread;
    std::atomic<bool> _running;
}
@property (nonatomic, readwrite) SPPlayerState state;
@property (nonatomic, readwrite) BOOL isLive;
@property (nonatomic, readwrite) NSTimeInterval duration;
@property (nonatomic, strong) SPPlayerView *internalView;
@end

@implementation SPPlayer

- (instancetype)init {
    self = [super init];
    if (self) {
        _state = SPPlayerStateIdle;
        _videoPktQueue = sp::PacketQueue(256);
        _audioPktQueue = sp::PacketQueue(256);
        _videoFrameQueue = sp::FrameQueue(8);
        _audioFrameQueue = sp::FrameQueue(32);
        _running = false;
        _internalView = [[SPPlayerView alloc] initWithFrame:CGRectMake(0, 0, 320, 240)];
    }
    return self;
}

- (UIView *)playerView {
    return _internalView;
}

- (BOOL)openURL:(NSString *)url {
    self.state = SPPlayerStatePreparing;
    std::string urlStr = [url UTF8String];

    _demuxer = std::make_unique<sp::Demuxer>(_videoPktQueue, _audioPktQueue);
    if (!_demuxer->open(urlStr)) {
        self.state = SPPlayerStateError;
        return NO;
    }

    const auto& info = _demuxer->media_info();
    self.isLive = info.is_live;
    self.duration = info.duration;

    // VideoToolbox will be selected automatically by DecoderFactory on iOS
    if (info.video_codecpar) {
        _videoDecoder = sp::DecoderFactory::create(
            _videoPktQueue, _videoFrameQueue, info.video_codecpar,
            sp::DecoderType::Auto, "VDec");
        if (!_videoDecoder) {
            self.state = SPPlayerStateError;
            return NO;
        }

        // Init GL renderer
        [EAGLContext setCurrentContext:_internalView.glContext];
        _glRender = std::make_unique<sp::GLVideoRender>();
        _glRender->init(info.video_codecpar->width, info.video_codecpar->height);
    }

    if (info.audio_codecpar) {
        _audioDecoder = sp::DecoderFactory::create(
            _audioPktQueue, _audioFrameQueue, info.audio_codecpar,
            sp::DecoderType::Software, "ADec");
        if (!_audioDecoder) {
            self.state = SPPlayerStateError;
            return NO;
        }
    }

    SP_LOGI("iOS", "Opened: %s", urlStr.c_str());
    return YES;
}

- (void)play {
    if (self.state == SPPlayerStatePaused) {
        self.state = SPPlayerStatePlaying;
        return;
    }

    _demuxer->start();
    if (_videoDecoder) _videoDecoder->start();
    if (_audioDecoder) _audioDecoder->start();

    _running = true;
    self.state = SPPlayerStatePlaying;

    // Video render thread
    if (_glRender) {
        _renderThread = std::thread([self] {
            [self renderLoop];
        });
    }
}

- (void)renderLoop {
    [EAGLContext setCurrentContext:_internalView.glContext];
    AVFrame* frame = av_frame_alloc();

    while (_running) {
        if (self.state == SPPlayerStatePaused) {
            usleep(10000);
            continue;
        }

        if (!_videoFrameQueue.pop(frame)) break;

        glBindFramebuffer(GL_FRAMEBUFFER, _internalView.framebuffer);
        _glRender->render(frame);
        [_internalView present];
        av_frame_unref(frame);
    }

    av_frame_free(&frame);
}

- (void)pause {
    self.state = SPPlayerStatePaused;
}

- (void)stop {
    _running = false;
    self.state = SPPlayerStateStopped;

    _videoPktQueue.abort();
    _audioPktQueue.abort();
    _videoFrameQueue.abort();
    _audioFrameQueue.abort();

    if (_renderThread.joinable()) _renderThread.join();
    if (_demuxer) _demuxer->stop();
    if (_videoDecoder) _videoDecoder->stop();
    if (_audioDecoder) _audioDecoder->stop();
}

- (void)seekTo:(NSTimeInterval)position {
    if (self.isLive || !_demuxer) return;
    _videoFrameQueue.flush();
    _audioFrameQueue.flush();
    if (_videoDecoder) _videoDecoder->flush();
    if (_audioDecoder) _audioDecoder->flush();
    _demuxer->seek(position);
}

- (void)dealloc {
    [self stop];
}

@end
