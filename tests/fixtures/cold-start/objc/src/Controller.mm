namespace presentation {
template<class T> T doubled(T value) { return value + value; }
}

@interface SandPanel
- (int)showAccount:(int)value;
@end

@implementation SandPanel
- (int)showAccount:(int)value {
    return presentation::doubled(value);
}
@end
