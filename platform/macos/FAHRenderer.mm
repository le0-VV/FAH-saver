#import "FAHRenderer.h"
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

static NSString * const FAHMetalSource = @R"metal(
#include <metal_stdlib>
using namespace metal;
struct Vertex { packed_float3 position; packed_float3 normal; };
struct Instance { float4x4 transform; float4 colour; };
struct Uniforms { float4x4 matrix; float4 parameters; float4 background; };
struct Varying { float4 position [[position]]; float3 normal; float3 colour; };
vertex Varying proteinVertex(uint index [[vertex_id]], uint instance [[instance_id]],
    const device Vertex *vertices [[buffer(0)]],
    const device Instance *instances [[buffer(1)]], constant Uniforms &u [[buffer(2)]]) {
    Vertex v = vertices[index];
    Instance item = instances[instance];
    float3 n = normalize((item.transform * float4(float3(v.normal), 0)).xyz);
    Varying out;
    out.position = u.matrix * item.transform * float4(float3(v.position), 1);
    out.normal = float3(u.parameters.x*n.x + u.parameters.y*n.z,
                        n.y, -u.parameters.y*n.x + u.parameters.x*n.z);
    out.colour = item.colour.xyz;
    return out;
}
fragment float4 proteinFragment(Varying in [[stage_in]]) {
    float light = .25 + .75 * max(dot(normalize(in.normal), normalize(float3(.4,.8,1))), 0.0);
    return float4(in.colour * light, 1);
}
struct Background { float4 position [[position]]; float2 uv; };
vertex Background backgroundVertex(uint index [[vertex_id]]) {
    float2 p = index == 0 ? float2(-1,-1) : (index == 1 ? float2(3,-1) : float2(-1,3));
    return { float4(p, .99999, 1), float2(p.x*.5+.5, .5-p.y*.5) };
}
fragment float4 backgroundFragment(Background in [[stage_in]], constant Uniforms &u [[buffer(0)]]) {
    if (u.parameters.z < .5) return float4(u.background.xyz, 1);
    float3 colour = mix(float3(.09,.23,.40), float3(.55,.70,.80), in.uv.y);
    if (in.uv.y > .55) {
        float depth = (in.uv.y-.55)/.45;
        float2 p = float2((in.uv.x-.5)*6.,3.)/max(depth,.02);
        float2 grid = abs(fract(p-.5)-.5)/max(fwidth(p),float2(.0001));
        float line = 1.-min(min(grid.x,grid.y),1.);
        colour = mix(colour,float3(.25,.38,.45),line*depth*.35);
    }
    return float4(colour, 1);
}
)metal";

struct FAHUniforms { float matrix[16]; float parameters[4]; float background[4]; };
static_assert(sizeof(fah::Vertex) == 24 && sizeof(fah::Instance) == 80);

static NSDictionary *summary(const std::vector<double>& values) {
    if (values.empty()) return @{ @"samples": @0 };
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    auto percentile = [&](double p) { return sorted[std::min(sorted.size()-1,
        static_cast<std::size_t>(std::ceil(p*sorted.size())-1))]; };
    double sum = 0;
    for (double value : sorted) sum += value;
    return @{ @"samples": @(sorted.size()), @"mean": @(sum/sorted.size()),
        @"p50": @(percentile(.5)), @"p95": @(percentile(.95)),
        @"p99": @(percentile(.99)), @"max": @(sorted.back()) };
}

@implementation FAHRenderer {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _queue;
    id<MTLRenderPipelineState> _proteinPipeline, _backgroundPipeline;
    id<MTLDepthStencilState> _depthState, _backgroundDepth;
    id<MTLBuffer> _sphereVertices, _sphereIndices, _cylinderVertices, _cylinderIndices;
    id<MTLBuffer> _atomInstances, _bondInstances;
    id<MTLTexture> _depth;
    id<MTLCommandBuffer> _lastCommand;
    std::size_t _sphereIndexCount, _cylinderIndexCount, _atomCount, _bondCount;
    std::shared_ptr<const fah::Capture> _capture;
    FAHRenderOptions _options;
    fah::Scene _scene;
    bool _dirty, _running;
    std::size_t _frame, _styleStep;
    fah::Style _style;
    double _firstTime;
    NSString *_lastError;
    std::vector<double> _cpuMilliseconds, _gpuMilliseconds, _presentedTimes;
    NSUInteger _submitted, _completed, _gpuErrors, _droppedMetricSamples;
}

- (instancetype)initWithDevice:(id<MTLDevice>)device error:(NSError **)error {
    self = [super init];
    if (!self) return nil;
    _device = device;
    _firstTime = -1;
    _styleStep = std::numeric_limits<std::size_t>::max();
    _style = fah::Style::SpaceFilling;
    _lastError = @"No protein snapshots available.";
    if (!device) {
        if (error) *error = [NSError errorWithDomain:@"FAHSaver" code:1
            userInfo:@{NSLocalizedDescriptionKey: @"Metal is unavailable on this computer."}];
        return nil;
    }
    _queue = [device newCommandQueue];
    id<MTLLibrary> library = [device newLibraryWithSource:FAHMetalSource options:nil error:error];
    if (!library || !_queue) return nil;
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    descriptor.vertexFunction = [library newFunctionWithName:@"proteinVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"proteinFragment"];
    _proteinPipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:error];
    if (!_proteinPipeline) return nil;
    descriptor.vertexFunction = [library newFunctionWithName:@"backgroundVertex"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"backgroundFragment"];
    _backgroundPipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:error];
    if (!_backgroundPipeline) return nil;
    MTLDepthStencilDescriptor *depthDescriptor = [MTLDepthStencilDescriptor new];
    depthDescriptor.depthCompareFunction = MTLCompareFunctionLess;
    depthDescriptor.depthWriteEnabled = YES;
    _depthState = [device newDepthStencilStateWithDescriptor:depthDescriptor];
    depthDescriptor.depthCompareFunction = MTLCompareFunctionAlways;
    depthDescriptor.depthWriteEnabled = NO;
    _backgroundDepth = [device newDepthStencilStateWithDescriptor:depthDescriptor];
    auto sphere = fah::sphereMesh();
    auto cylinder = fah::cylinderMesh();
    _sphereVertices = [device newBufferWithBytes:sphere.vertices.data()
        length:sphere.vertices.size()*sizeof(fah::Vertex) options:MTLResourceStorageModeShared];
    _sphereIndices = [device newBufferWithBytes:sphere.indices.data()
        length:sphere.indices.size()*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
    _cylinderVertices = [device newBufferWithBytes:cylinder.vertices.data()
        length:cylinder.vertices.size()*sizeof(fah::Vertex) options:MTLResourceStorageModeShared];
    _cylinderIndices = [device newBufferWithBytes:cylinder.indices.data()
        length:cylinder.indices.size()*sizeof(std::uint32_t) options:MTLResourceStorageModeShared];
    if (!_sphereVertices || !_sphereIndices || !_cylinderVertices || !_cylinderIndices) {
        if (error) *error = [NSError errorWithDomain:@"FAHSaver" code:2
            userInfo:@{NSLocalizedDescriptionKey: @"Could not allocate Metal mesh buffers."}];
        return nil;
    }
    _sphereIndexCount = sphere.indices.size();
    _cylinderIndexCount = cylinder.indices.size();
    return self;
}

- (id<MTLDevice>)device { return _device; }
- (NSString *)lastError { @synchronized(self) { return _lastError; } }

- (void)setCapture:(std::shared_ptr<const fah::Capture>)capture {
    _capture = std::move(capture);
    _dirty = true;
    _frame = 0;
    @synchronized(self) {
        _lastError = (!_capture || _capture->frames.empty()) ? @"No protein snapshots available." : nil;
    }
}

- (void)setOptions:(const FAHRenderOptions&)options {
    if (options.fps < 1 || options.fps > 240 || !std::isfinite(options.snapshotInterval) ||
        options.snapshotInterval <= 0 || !std::isfinite(options.styleInterval) || options.styleInterval <= 0)
        throw std::runtime_error("Invalid render timing settings");
    const int style = static_cast<int>(options.style);
    if (style < 1 || style > 3) throw std::runtime_error("Model style must be 1, 2 or 3");
    for (float value : {options.colour.x,options.colour.y,options.colour.z})
        if (!std::isfinite(value) || value < 0 || value > 1)
            throw std::runtime_error("Background colour components must be between zero and one");
    _options = options;
    _style = options.style;
    _styleStep = std::numeric_limits<std::size_t>::max();
    _dirty = true;
}

- (void)start { _running = true; _firstTime = -1; }
- (void)stop {
    _running = false;
    [_lastCommand waitUntilCompleted];
    _lastCommand = nil;
    _atomInstances = nil;
    _bondInstances = nil;
    _depth = nil;
    _scene = {};
    _atomCount = _bondCount = 0;
    _dirty = true;
}

- (void)updateSceneAtTime:(double)elapsed {
    if (!_capture || _capture->frames.empty()) return;
    const std::size_t frame = _options.loop ?
        static_cast<std::size_t>(elapsed/_options.snapshotInterval) % _capture->frames.size() :
        _capture->frames.size()-1;
    const std::size_t step = static_cast<std::size_t>(elapsed/_options.styleInterval);
    auto style = _style;
    if (step != _styleStep) {
        if (_options.styleMode == FAHStyleMode::Cycle)
            style = static_cast<fah::Style>((static_cast<int>(_options.style)-1+step)%3+1);
        else if (_options.styleMode == FAHStyleMode::Random && _styleStep != std::numeric_limits<std::size_t>::max())
            style = static_cast<fah::Style>((static_cast<int>(_style)-1+1+arc4random_uniform(2))%3+1);
        _styleStep = step;
    }
    if (!_dirty && frame == _frame && style == _style) return;
    // The shared instance buffers must not change while the previous draw uses them.
    [_lastCommand waitUntilCompleted];
    auto scene = fah::makeScene(_capture->topology, _capture->frames[frame], style);
    auto upload = [&](id<MTLBuffer> old, const std::vector<fah::Instance>& instances) -> id<MTLBuffer> {
        if (instances.empty()) return nil;
        const NSUInteger bytes = instances.size()*sizeof(fah::Instance);
        id<MTLBuffer> buffer = old;
        if (!buffer || buffer.length < bytes)
            buffer = [_device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        if (!buffer) throw std::runtime_error("Could not allocate Metal instance buffers");
        std::memcpy(buffer.contents, instances.data(), bytes);
        return buffer;
    };
    _atomInstances = upload(_atomInstances, scene.atoms);
    _bondInstances = upload(_bondInstances, scene.bonds);
    _atomCount = scene.atoms.size();
    _bondCount = scene.bonds.size();
    _scene = std::move(scene);
    _frame = frame;
    _style = style;
    _dirty = false;
}

- (id<MTLCommandBuffer>)renderToTexture:(id<MTLTexture>)texture time:(double)time
                            drawable:(id<CAMetalDrawable>)drawable {
    if (!_running || !texture) return nil;
    const bool hasModel = _capture && !_capture->frames.empty();
    if (!hasModel && !_dirty) return nil;
    if (!hasModel) { _atomCount = _bondCount = 0; _scene.radius = 1; _dirty = false; }
    if (!std::isfinite(time)) return nil;
    const double cpuStart = CACurrentMediaTime();
    if (_firstTime < 0) _firstTime = time;
    const double elapsed = std::max(0.0, time-_firstTime);
    try {
        [self updateSceneAtTime:elapsed];
    } catch (const std::exception& exception) {
        @synchronized(self) { _lastError = [NSString stringWithUTF8String:exception.what()]; }
        return nil;
    }
    if (!_depth || _depth.width != texture.width || _depth.height != texture.height) {
        MTLTextureDescriptor *descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
            MTLPixelFormatDepth32Float width:texture.width height:texture.height mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageRenderTarget;
        _depth = [_device newTextureWithDescriptor:descriptor];
        if (!_depth) { @synchronized(self) { _lastError = @"Could not allocate the depth texture."; } return nil; }
    }
    id<MTLCommandBuffer> command = [_queue commandBuffer];
    if (!command) { @synchronized(self) { _lastError = @"Could not create a Metal command buffer."; } return nil; }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(.02,.025,.035,1);
    pass.depthAttachment.texture = _depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    pass.depthAttachment.clearDepth = 1;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!encoder) { @synchronized(self) { _lastError = @"Could not create a Metal render encoder."; } return nil; }
    const float angle = static_cast<float>(elapsed*.2);
    auto matrix = fah::viewProjection(angle, static_cast<float>(texture.width)/texture.height, _scene.radius);
    FAHUniforms uniforms{};
    std::copy(matrix.begin(), matrix.end(), uniforms.matrix);
    uniforms.parameters[0] = std::cos(angle);
    uniforms.parameters[1] = std::sin(angle);
    uniforms.parameters[2] = _options.sky ? 1 : 0;
    uniforms.background[0] = _options.colour.x;
    uniforms.background[1] = _options.colour.y;
    uniforms.background[2] = _options.colour.z;
    [encoder setCullMode:MTLCullModeNone];
    [encoder setRenderPipelineState:_backgroundPipeline];
    [encoder setDepthStencilState:_backgroundDepth];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder setRenderPipelineState:_proteinPipeline];
    [encoder setDepthStencilState:_depthState];
    [encoder setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:2];
    if (_atomCount) {
        [encoder setVertexBuffer:_sphereVertices offset:0 atIndex:0];
        [encoder setVertexBuffer:_atomInstances offset:0 atIndex:1];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:_sphereIndexCount
            indexType:MTLIndexTypeUInt32 indexBuffer:_sphereIndices indexBufferOffset:0 instanceCount:_atomCount];
    }
    if (_bondCount) {
        [encoder setVertexBuffer:_cylinderVertices offset:0 atIndex:0];
        [encoder setVertexBuffer:_bondInstances offset:0 atIndex:1];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:_cylinderIndexCount
            indexType:MTLIndexTypeUInt32 indexBuffer:_cylinderIndices indexBufferOffset:0 instanceCount:_bondCount];
    }
    [encoder endEncoding];
    const bool metrics = _options.collectMetrics;
    if (metrics) {
        @synchronized(self) {
            ++_submitted;
            if (_cpuMilliseconds.size() < 1000000) _cpuMilliseconds.push_back((CACurrentMediaTime()-cpuStart)*1000);
            else ++_droppedMetricSamples;
        }
        if (drawable) [drawable addPresentedHandler:^(id<MTLDrawable> presented) {
            @synchronized(self) {
                if (presented.presentedTime > 0 && self->_presentedTimes.size() < 1000000)
                    self->_presentedTimes.push_back(presented.presentedTime);
            }
        }];
    }
    [command addCompletedHandler:^(id<MTLCommandBuffer> finished) {
        @synchronized(self) {
            if (metrics) ++self->_completed;
            if (finished.status == MTLCommandBufferStatusError) {
                ++self->_gpuErrors;
                self->_lastError = finished.error.localizedDescription;
            }
            if (metrics && finished.GPUEndTime > finished.GPUStartTime && self->_gpuMilliseconds.size() < 1000000)
                self->_gpuMilliseconds.push_back((finished.GPUEndTime-finished.GPUStartTime)*1000);
        }
    }];
    if (drawable) [command presentDrawable:drawable];
    [command commit];
    _lastCommand = command;
    return command;
}

- (void)drawInMTKView:(MTKView *)view {
    if (!_running) return;
    @autoreleasepool {
        id<CAMetalDrawable> drawable = view.currentDrawable;
        if (drawable) [self renderToTexture:drawable.texture time:CACurrentMediaTime() drawable:drawable];
    }
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {}

- (NSDictionary *)metrics {
    @synchronized(self) {
        std::vector<double> intervals;
        auto times = _presentedTimes;
        std::sort(times.begin(),times.end());
        for (std::size_t i=1; i<times.size(); ++i) intervals.push_back((times[i]-times[i-1])*1000);
        double fps = times.size()>1 && times.back()>times.front() ? (times.size()-1)/(times.back()-times.front()) : 0;
        return @{ @"renderer": @"Metal", @"gpu": _device.name ? _device.name : @"unavailable",
            @"measurement": times.empty() ? @"offscreen submissions; no presentation measurement" : @"drawable presentation timestamps",
            @"requested_fps": @(_options.fps), @"submitted_frames": @(_submitted),
            @"completed_frames": @(_completed), @"gpu_errors": @(_gpuErrors),
            @"presented_frames": @(times.size()), @"delivered_fps": @(fps),
            @"cpu_encode_ms": summary(_cpuMilliseconds), @"gpu_ms": summary(_gpuMilliseconds),
            @"presentation_interval_ms": summary(intervals), @"dropped_metric_samples": @(_droppedMetricSamples),
            @"snapshot_count": @(_capture ? _capture->frames.size() : 0),
            @"atom_count": @(_capture ? _capture->topology.atoms.size() : 0),
            @"bond_count": @(_capture ? _capture->topology.bonds.size() : 0) };
    }
}
@end
