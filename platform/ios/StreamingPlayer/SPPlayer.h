#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, SPPlayerState) {
    SPPlayerStateIdle,
    SPPlayerStatePreparing,
    SPPlayerStatePlaying,
    SPPlayerStatePaused,
    SPPlayerStateStopped,
    SPPlayerStateError
};

@protocol SPPlayerDelegate <NSObject>
@optional
- (void)playerDidChangeState:(SPPlayerState)state;
- (void)playerDidReachEnd;
- (void)playerDidFailWithError:(NSError *)error;
@end

/// ObjC bridge to the C++ streaming player core.
/// Uses VideoToolbox for hardware-accelerated H.264/H.265 decoding.
@interface SPPlayer : NSObject

@property (nonatomic, readonly) SPPlayerState state;
@property (nonatomic, readonly) BOOL isLive;
@property (nonatomic, readonly) NSTimeInterval duration;
@property (nonatomic, weak, nullable) id<SPPlayerDelegate> delegate;

/// The view that displays the video. Add to your view hierarchy.
@property (nonatomic, readonly) UIView *playerView;

- (BOOL)openURL:(NSString *)url;
- (void)play;
- (void)pause;
- (void)stop;
- (void)seekTo:(NSTimeInterval)position;

@end

NS_ASSUME_NONNULL_END
