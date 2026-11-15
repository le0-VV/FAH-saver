(() => {
function sphereMesh() {
  const vertices = [], indices = [];
  const segments = 16, rings = 12;
  for (let y = 0; y <= rings; y++) {
    const phi = Math.PI * y / rings;
    for (let x = 0; x <= segments; x++) {
      const theta = 2 * Math.PI * x / segments;
      const position = [Math.sin(phi) * Math.cos(theta), Math.cos(phi), Math.sin(phi) * Math.sin(theta)];
      vertices.push(...position, ...position);
    }
  }
  for (let y = 0; y < rings; y++) for (let x = 0; x < segments; x++) {
    const a = y * (segments + 1) + x, b = a + segments + 1;
    if (y !== 0) indices.push(a, a + 1, b);
    if (y !== rings - 1) indices.push(a + 1, b + 1, b);
  }
  return {vertices: new Float32Array(vertices), indices: new Uint32Array(indices)};
}

function cylinderMesh() {
  const vertices = [], indices = [], segments = 12;
  for (let i = 0; i <= segments; i++) {
    const angle = 2 * Math.PI * i / segments, x = Math.cos(angle), z = Math.sin(angle);
    vertices.push(x, 0, z, x, 0, z, x, 1, z, x, 0, z);
  }
  for (let i = 0; i < segments; i++) {
    const a = i * 2;
    indices.push(a, a + 1, a + 2, a + 1, a + 3, a + 2);
  }
  for (const y of [0, 1]) {
    const centre = vertices.length / 6, normal = y === 0 ? -1 : 1;
    vertices.push(0, y, 0, 0, normal, 0);
    for (let i = 0; i <= segments; i++) {
      const angle = 2 * Math.PI * i / segments;
      vertices.push(Math.cos(angle), y, Math.sin(angle), 0, normal, 0);
    }
    for (let i = 0; i < segments; i++) {
      if (y === 0) indices.push(centre, centre + i + 1, centre + i + 2);
      else indices.push(centre, centre + i + 2, centre + i + 1);
    }
  }
  return {vertices: new Float32Array(vertices), indices: new Uint32Array(indices)};
}

function cross(a, b) {
  return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
}
function normalise(v) { const length = Math.hypot(...v); return v.map(x => x / length); }

function makeScene(capture, frameIndex, style) {
  if (![1, 2, 3].includes(style)) throw new Error('Style must be 1, 2 or 3');
  const frame = capture.frames[frameIndex];
  if (!frame) throw new Error('Frame index is out of range');
  const framing = capture.bonds.length ? new Uint8Array(capture.atoms.length) : null;
  if (framing) for (const [a, b] of capture.bonds) { framing[a] = 1; framing[b] = 1; }
  const centre = [0, 0, 0];
  let framingAtoms = 0;
  for (let atom = 0; atom < capture.atoms.length; atom++) if (!framing || framing[atom]) {
    framingAtoms++;
    for (let axis = 0; axis < 3; axis++) centre[axis] += frame[atom * 3 + axis];
  }
  for (let axis = 0; axis < 3; axis++) centre[axis] = Math.fround(centre[axis] / framingAtoms);
  const atoms = new Float32Array(capture.atoms.length * 20), bonds = [];
  let radius = 0;
  capture.atoms.forEach((atom, i) => {
    const x = frame[3 * i] - centre[0], y = frame[3 * i + 1] - centre[1], z = frame[3 * i + 2] - centre[2];
    const scale = style === 1 ? atom.radius : style === 2 ? atom.radius / 3 : .025;
    atoms.set([scale, 0, 0, 0, 0, scale, 0, 0, 0, 0, scale, 0, x, y, z, 1, ...atom.colour, 1], i * 20);
    if (!framing || framing[i]) radius = Math.max(radius, Math.hypot(x, y, z) + atom.radius);
  });
  if (style !== 1) for (const [a, b] of capture.bonds) {
    const start = [frame[3 * a] - centre[0], frame[3 * a + 1] - centre[1], frame[3 * a + 2] - centre[2]];
    const direction = [frame[3 * b] - frame[3 * a], frame[3 * b + 1] - frame[3 * a + 1], frame[3 * b + 2] - frame[3 * a + 2]];
    const length = Math.hypot(...direction);
    if (length === 0) continue;
    const axis = direction.map(x => x / length);
    const reference = Math.abs(axis[1]) < .9 ? [0, 1, 0] : [1, 0, 0];
    const right = normalise(cross(axis, reference)), front = cross(right, axis);
    bonds.push(right[0] * .01, right[1] * .01, right[2] * .01, 0,
      ...direction, 0, front[0] * .01, front[1] * .01, front[2] * .01, 0,
      ...start, 1, .75, .75, .75, 1);
  }
  return {atoms, bonds: new Float32Array(bonds), radius: Math.max(radius, .001)};
}

function multiply(a, b) {
  const result = new Float32Array(16);
  for (let column = 0; column < 4; column++) for (let row = 0; row < 4; row++) {
    for (let k = 0; k < 4; k++) result[column * 4 + row] += a[k * 4 + row] * b[column * 4 + k];
  }
  return result;
}

function viewProjection(angle, aspect, radius) {
  if (!Number.isFinite(angle) || !Number.isFinite(aspect) || aspect <= 0 || !Number.isFinite(radius) || radius <= 0)
    throw new Error('Invalid camera dimensions');
  const distance = radius * 1.1 * Math.max(1, 1 / aspect) / Math.sin(Math.PI / 8);
  const near = Math.max(radius * .01, distance - radius * 1.1), far = distance + radius * 1.1;
  const f = 1 / Math.tan(Math.PI / 8), c = Math.cos(angle), s = Math.sin(angle);
  const projection = new Float32Array([f / aspect, 0, 0, 0, 0, f, 0, 0, 0, 0, far / (near - far), -1, 0, 0, far * near / (near - far), 0]);
  const viewModel = new Float32Array([c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, -distance, 1]);
  return multiply(projection, viewModel);
}

function styleAt(mode, seconds) {
  if (/^[123]$/.test(String(mode))) return Number(mode);
  const period = Math.floor(seconds / 10);
  if (mode === 'cycle') return period % 3 + 1;
  if (mode === 'random') {
    let hash = (period + 1) >>> 0;
    hash = Math.imul(hash ^ (hash >>> 16), 0x45d9f3b);
    hash = Math.imul(hash ^ (hash >>> 16), 0x45d9f3b);
    return ((hash ^ (hash >>> 16)) >>> 0) % 3 + 1;
  }
  throw new Error('Style must be 1, 2, 3, cycle or random');
}

function nextFrameDeadline(now, due, fps) {
  const interval = 1000 / fps;
  if (now + Math.min(1.5, interval * .1) < due) return null;
  return due + (Math.floor(Math.max(0, now - due) / interval) + 1) * interval;
}

globalThis.FAHScene = {sphereMesh, cylinderMesh, makeScene, viewProjection, styleAt, nextFrameDeadline};
})();
