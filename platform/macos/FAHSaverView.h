#import <ScreenSaver/ScreenSaver.h>
#import "FAHRenderer.h"

@interface FAHSaverView : ScreenSaverView
@property(nonatomic, readonly) BOOL hasActiveSource;
@property(nonatomic, readonly) NSUInteger sourceCallbackCount;
- (instancetype)initWithFrame:(NSRect)frame isPreview:(BOOL)isPreview preferences:(NSUserDefaults *)preferences;
- (void)setCapture:(std::shared_ptr<const fah::Capture>)capture;
@end
