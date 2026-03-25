#import "ViewController.h"
#import "SPPlayer.h"

@interface ViewController () <SPPlayerDelegate>
@property (nonatomic, strong) SPPlayer *player;
@property (nonatomic, strong) UIButton *playButton;
@property (nonatomic, strong) UILabel *statusLabel;
@end

@implementation ViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.blackColor;

    // Player
    self.player = [[SPPlayer alloc] init];
    self.player.delegate = self;

    // Video view
    UIView *playerView = self.player.playerView;
    playerView.frame = CGRectMake(0, 0, self.view.bounds.size.width,
                                   self.view.bounds.size.height - 60);
    playerView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    [self.view addSubview:playerView];

    // Controls
    self.playButton = [UIButton buttonWithType:UIButtonTypeSystem];
    self.playButton.frame = CGRectMake(20, self.view.bounds.size.height - 50, 80, 40);
    [self.playButton setTitle:@"Play" forState:UIControlStateNormal];
    [self.playButton setTitleColor:UIColor.whiteColor forState:UIControlStateNormal];
    [self.playButton addTarget:self action:@selector(togglePlay) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:self.playButton];

    self.statusLabel = [[UILabel alloc] initWithFrame:
        CGRectMake(120, self.view.bounds.size.height - 50, 200, 40)];
    self.statusLabel.textColor = UIColor.whiteColor;
    self.statusLabel.text = @"Ready";
    [self.view addSubview:self.statusLabel];
}

- (void)togglePlay {
    if (self.player.state != SPPlayerStatePlaying) {
        NSString *url = @"http://127.0.0.1:8080/live/stream.m3u8";
        if ([self.player openURL:url]) {
            [self.player play];
            [self.playButton setTitle:@"Stop" forState:UIControlStateNormal];
            self.statusLabel.text = self.player.isLive ? @"LIVE" : @"Playing";
        } else {
            self.statusLabel.text = @"Failed to open";
        }
    } else {
        [self.player stop];
        [self.playButton setTitle:@"Play" forState:UIControlStateNormal];
        self.statusLabel.text = @"Stopped";
    }
}

- (void)playerDidChangeState:(SPPlayerState)state {
    // Handle state changes on main thread if needed
}

@end
