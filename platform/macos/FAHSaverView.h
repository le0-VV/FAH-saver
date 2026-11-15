#import <ScreenSaver/ScreenSaver.h>
#import "FAHRenderer.h"

@interface FAHSaverView : ScreenSaverView
- (void)setCapture:(std::shared_ptr<const fah::Capture>)capture;
@end
