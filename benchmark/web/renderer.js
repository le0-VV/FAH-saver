(() => {
  'use strict';
  const {sphereMesh, cylinderMesh, makeScene, viewProjection, styleAt} = globalThis.FAHScene;
  const defaults = Object.freeze({fps: 60, style: 1, styleMode: 'fixed', playback: 'latest',
    background: 'sky', colour: Object.freeze([.02, .025, .035]), snapshotInterval: 1, styleInterval: 10, collectMetrics: false, frameDriver: 'raf'});

  function normaliseOptions(input = {}, previous = defaults) {
    if (!input || typeof input !== 'object' || Array.isArray(input)) throw new Error('Renderer options must be an object');
    for (const key of Object.keys(input)) if (!Object.prototype.hasOwnProperty.call(defaults, key)) throw new Error(`Unknown renderer option: ${key}`);
    const next = {...previous, ...input};
    if (!Number.isInteger(next.fps) || next.fps < 1 || next.fps > 240) throw new Error('Frame limit must be an integer from 1 to 240');
    if (!Number.isInteger(next.style) || next.style < 1 || next.style > 3) throw new Error('Style must be 1, 2 or 3');
    if (!['fixed', 'cycle', 'random'].includes(next.styleMode)) throw new Error('Style mode must be fixed, cycle or random');
    if (!['latest', 'loop'].includes(next.playback)) throw new Error('Playback must be latest or loop');
    if (!['sky', 'dark', 'solid'].includes(next.background)) throw new Error('Background must be sky, dark or solid');
    if (!Array.isArray(next.colour) || next.colour.length !== 3 || !next.colour.every(x => typeof x === 'number' && Number.isFinite(x) && x >= 0 && x <= 1))
      throw new Error('Colour must have three components between zero and one');
    if (!Number.isFinite(next.snapshotInterval) || next.snapshotInterval < .05) throw new Error('Snapshot interval must be at least 0.05 seconds');
    if (!Number.isFinite(next.styleInterval) || next.styleInterval <= 0) throw new Error('Style interval must be positive');
    if (!['raf', 'native'].includes(next.frameDriver)) throw new Error('Frame driver must be raf or native');
    if (typeof next.collectMetrics !== 'boolean') throw new Error('collectMetrics must be boolean');
    next.colour = next.colour.slice();
    return next;
  }
  const vertexSource = `#version 300 es
    precision highp float;
    layout(location=0) in vec3 position;
    layout(location=1) in vec3 normal;
    layout(location=2) in mat4 instance;
    layout(location=6) in vec4 colour;
    uniform mat4 viewProjection;
    uniform mat3 rotation;
    out vec3 vertexNormal;
    out vec4 vertexColour;
    void main() {
      vec4 clip = viewProjection * instance * vec4(position, 1.);
      clip.z = 2. * clip.z - clip.w;
      gl_Position = clip;
      vertexNormal = rotation * normalize(mat3(instance) * normal);
      vertexColour = colour;
    }`;
  const fragmentSource = `#version 300 es
    precision highp float;
    in vec3 vertexNormal;
    in vec4 vertexColour;
    out vec4 outputColour;
    void main() {
      float light = .25 + .75 * max(dot(normalize(vertexNormal), normalize(vec3(.4,.8,1.))), 0.);
      outputColour = vec4(vertexColour.rgb * light, 1.);
    }`;
  const backgroundVertexSource = `#version 300 es
    precision highp float;
    out vec2 uv;
    void main() {
      vec2 p = gl_VertexID == 0 ? vec2(-1.,-1.) : gl_VertexID == 1 ? vec2(3.,-1.) : vec2(-1.,3.);
      gl_Position = vec4(p,0.,1.);
      uv = vec2(p.x * .5 + .5, .5 - p.y * .5);
    }`;
  const backgroundFragmentSource = `#version 300 es
    precision highp float;
    in vec2 uv;
    uniform int sky;
    uniform vec3 solid;
    out vec4 outputColour;
    void main() {
      vec3 colour = solid;
      if (sky != 0) {
        bool dark = sky == 2;
        vec3 top = dark ? vec3(11.,18.,29.) / 255. : vec3(.09,.23,.40);
        vec3 bottom = dark ? vec3(20.,30.,44.) / 255. : vec3(.55,.70,.80);
        vec3 gridColour = dark ? vec3(80.,93.,108.) / 255. : vec3(.25,.38,.45);
        colour = mix(top, bottom, uv.y);
        if (uv.y > .55) {
          float depth = (uv.y - .55) / .45;
          vec2 p = vec2((uv.x - .5) * 6., 3.) / max(depth, .02);
          vec2 grid = abs(fract(p - .5) - .5) / max(fwidth(p), vec2(.0001));
          float line = 1. - min(min(grid.x, grid.y), 1.);
          colour = mix(colour, gridColour, line * depth * .35);
        }
      }
      outputColour = vec4(colour,1.);
    }`;

  function createRenderer(canvas, {onContextLost = () => {}} = {}) {
    let gl = canvas.getContext('webgl2', {alpha: false, antialias: false, depth: true, powerPreference: 'default'});
    if (!gl) throw new Error('WebGL 2 is unavailable');
    let proteinProgram, backgroundProgram, atomMesh, bondMesh, uniform, backgroundUniform;
    let capture, scene, previousPositions, previousStyle = -1, disposed = false, draws = 0;
    let settings = normaliseOptions();

    function requireActive() { if (disposed) throw new Error('Renderer has been disposed'); }
    function program(vertex, fragment) {
      const shaders = [], output = gl.createProgram();
      if (!output) throw new Error('Cannot allocate a WebGL program');
      try {
        for (const [kind, source] of [[gl.VERTEX_SHADER, vertex], [gl.FRAGMENT_SHADER, fragment]]) {
          const shader = gl.createShader(kind);
          if (!shader) throw new Error('Cannot allocate a WebGL shader');
          shaders.push(shader); gl.shaderSource(shader, source); gl.compileShader(shader);
          if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader) || 'Shader compilation failed');
          gl.attachShader(output, shader);
        }
        gl.linkProgram(output);
        if (!gl.getProgramParameter(output, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(output) || 'Shader linking failed');
        return output;
      } catch (error) { gl.deleteProgram(output); throw error; }
      finally { for (const shader of shaders) gl.deleteShader(shader); }
    }
    function uploadMesh(mesh) {
      const vao = gl.createVertexArray(), vertex = gl.createBuffer(), index = gl.createBuffer(), instances = gl.createBuffer();
      if (!vao || !vertex || !index || !instances) {
        for (const buffer of [vertex, index, instances]) if (buffer) gl.deleteBuffer(buffer);
        if (vao) gl.deleteVertexArray(vao);
        throw new Error('Cannot allocate WebGL mesh resources');
      }
      gl.bindVertexArray(vao);
      gl.bindBuffer(gl.ARRAY_BUFFER, vertex); gl.bufferData(gl.ARRAY_BUFFER, mesh.vertices, gl.STATIC_DRAW);
      for (let i = 0; i < 2; i++) {
        gl.enableVertexAttribArray(i); gl.vertexAttribPointer(i, 3, gl.FLOAT, false, 24, i * 12);
      }
      gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, index); gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, mesh.indices, gl.STATIC_DRAW);
      gl.bindBuffer(gl.ARRAY_BUFFER, instances);
      for (let i = 0; i < 5; i++) {
        gl.enableVertexAttribArray(2 + i); gl.vertexAttribPointer(2 + i, 4, gl.FLOAT, false, 80, i * 16); gl.vertexAttribDivisor(2 + i, 1);
      }
      gl.bindVertexArray(null);
      return {vao, vertex, index, instances, indexCount: mesh.indices.length, count: 0};
    }
    function setInstances(mesh, data) {
      gl.bindBuffer(gl.ARRAY_BUFFER, mesh.instances); gl.bufferData(gl.ARRAY_BUFFER, data, gl.DYNAMIC_DRAW);
      mesh.count = data.length / 20;
    }
    function resize(strict = false) {
      requireActive();
      const width = Math.max(1, Math.round(canvas.clientWidth * globalThis.devicePixelRatio));
      const height = Math.max(1, Math.round(canvas.clientHeight * globalThis.devicePixelRatio));
      if (canvas.width !== width || canvas.height !== height) {
        if (strict) throw new Error('Drawable dimensions changed during the trial');
        canvas.width = width; canvas.height = height;
      }
      gl.viewport(0, 0, canvas.width, canvas.height);
    }
    function draw(seconds, gpuTimer, strictSize = false) {
      requireActive();
      if (!Number.isFinite(seconds) || seconds < 0) throw new Error('Render time must be finite and non-negative');
      resize(strictSize);
      const frame = !capture ? null : settings.playback === 'latest' ? capture.frames.length - 1 : Math.floor(seconds / settings.snapshotInterval) % capture.frames.length;
      const style = settings.styleMode === 'fixed' ? settings.style : styleAt(settings.styleMode, seconds, settings.styleInterval, settings.style);
      if (capture && (capture.frames[frame] !== previousPositions || style !== previousStyle)) {
        scene = makeScene(capture, frame, style);
        setInstances(atomMesh, scene.atoms); setInstances(bondMesh, scene.bonds);
        previousPositions = capture.frames[frame]; previousStyle = style;
      }
      if (!capture) { scene = {radius: 1}; atomMesh.count = bondMesh.count = 0; }
      const angle = seconds / 5, c = Math.cos(angle), s = Math.sin(angle);
      const camera = viewProjection(angle, canvas.width / canvas.height, scene.radius);
      const rotation = new Float32Array([c, 0, -s, 0, 1, 0, s, 0, c]);
      const timed = gpuTimer?.begin();
      try {
        gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
        gl.disable(gl.DEPTH_TEST); gl.disable(gl.CULL_FACE); gl.useProgram(backgroundProgram); gl.bindVertexArray(null);
        gl.uniform1i(backgroundUniform.sky, settings.background === 'sky' ? 1 : settings.background === 'dark' ? 2 : 0);
        gl.uniform3fv(backgroundUniform.solid, settings.colour); gl.drawArrays(gl.TRIANGLES, 0, 3);
        gl.enable(gl.DEPTH_TEST); gl.enable(gl.CULL_FACE); gl.useProgram(proteinProgram);
        gl.uniformMatrix4fv(uniform.vp, false, camera); gl.uniformMatrix3fv(uniform.rotation, false, rotation);
        for (const mesh of [atomMesh, bondMesh]) if (mesh.count) {
          gl.bindVertexArray(mesh.vao); gl.drawElementsInstanced(gl.TRIANGLES, mesh.indexCount, gl.UNSIGNED_INT, 0, mesh.count);
        }
      } finally { if (timed) gpuTimer.end(); }
      draws++;
      return true;
    }
    function contextLost(event) {
      event.preventDefault();
      if (!disposed) onContextLost(new Error('WebGL context was lost. Reload before rendering again.'));
    }
    function dispose() {
      if (disposed) return;
      disposed = true; capture = scene = previousPositions = undefined;
      canvas.removeEventListener('webglcontextlost', contextLost);
      for (const mesh of [atomMesh, bondMesh]) if (mesh) {
        for (const buffer of [mesh.vertex, mesh.index, mesh.instances]) gl.deleteBuffer(buffer);
        gl.deleteVertexArray(mesh.vao);
      }
      if (proteinProgram) gl.deleteProgram(proteinProgram);
      if (backgroundProgram) gl.deleteProgram(backgroundProgram);
      gl.getExtension('WEBGL_lose_context')?.loseContext();
    }
    try {
      proteinProgram = program(vertexSource, fragmentSource); backgroundProgram = program(backgroundVertexSource, backgroundFragmentSource);
      uniform = {vp: gl.getUniformLocation(proteinProgram, 'viewProjection'), rotation: gl.getUniformLocation(proteinProgram, 'rotation')};
      backgroundUniform = {sky: gl.getUniformLocation(backgroundProgram, 'sky'), solid: gl.getUniformLocation(backgroundProgram, 'solid')};
      atomMesh = uploadMesh(sphereMesh()); bondMesh = uploadMesh(cylinderMesh());
      gl.disable(gl.BLEND); gl.depthFunc(gl.LESS); gl.frontFace(gl.CCW); gl.cullFace(gl.BACK);
      canvas.addEventListener('webglcontextlost', contextLost);
    } catch (error) { dispose(); throw error; }
    return {
      gl, resize, draw, dispose,
      configure(value) { requireActive(); settings = normaliseOptions(value, settings); return {...settings}; },
      setCapture(value) {
        requireActive();
        if (capture !== value) { previousPositions = undefined; previousStyle = -1; }
        capture = value;
      },
      info() {
        const debug = !disposed && gl.getExtension('WEBGL_debug_renderer_info');
        return {renderer: 'WebGL2', width: canvas.width, height: canvas.height, draws, disposed,
          adapter: disposed ? null : gl.getParameter(debug ? debug.UNMASKED_RENDERER_WEBGL : gl.RENDERER),
          contextAttributes: disposed ? null : gl.getContextAttributes(),
          snapshotCount: capture?.frames.length || 0,
          snapshotIndex: capture && previousPositions ? capture.frames.indexOf(previousPositions) : null,
          style: previousStyle > 0 ? previousStyle : null};
      }
    };
  }
  globalThis.FAHWebRenderer = {createRenderer, normaliseOptions, defaults};
})();
