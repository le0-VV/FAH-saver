import assert from 'node:assert/strict';
import './capture.js';
import './scene.js';
import './gpu-timer-test.mjs';

const {parseCapture, validateCapture, readCaptureURL, limits} = globalThis.FAHCapture;
const {makeScene, sphereMesh, cylinderMesh, viewProjection, styleAt, elapsedSeconds, nextFrameDeadline} = globalThis.FAHScene;

// Synthetic geometry checks; this fixture is never loaded by the renderer itself.
const fixture = {
  topology: {atoms: [['C', 0, 0, 0, 6], ['O', 0, 0, 0, 8]], bonds: [[0, 1]]},
  frames: [[[0, 0, 0], [1, 0, 0]], [[0, 0, 0], [0, 1, 0]]]
};
const capture = parseCapture(JSON.stringify(fixture));
assert.equal(capture.atoms.length, 2);
assert.equal(capture.frames.length, 2);
assert.equal(capture.atoms[0].radius, .17);
assert.equal(makeScene(capture, 0, 1).bonds.length, 0);
const full = makeScene(capture, 0, 1), small = makeScene(capture, 0, 2), sticks = makeScene(capture, 0, 3);
assert.equal(full.atoms.length, 40);
assert.equal(small.bonds.length, 20);
assert.ok(Math.abs(full.atoms[0] / small.atoms[0] - 3) < 1e-6);
assert.ok(Math.abs(sticks.atoms[0] - .025) < 1e-6);
assert.equal(full.radius, small.radius);
assert.equal(full.radius, sticks.radius);
assert.equal(full.atoms[12], -.5);
assert.equal(full.atoms[32], .5);
assert.equal(small.bonds[13], 0);
assert.ok([...small.atoms, ...small.bonds, ...viewProjection(1, 16 / 9, full.radius)].every(Number.isFinite));

const duplicate = structuredClone(fixture);
duplicate.frames[0][1] = [0, 0, 0];
assert.equal(makeScene(validateCapture(duplicate), 0, 2).bonds.length, 0);
const outlier = structuredClone(fixture);
outlier.topology.atoms.push(['H']);
outlier.frames.forEach(frame => frame.push([100, 0, 0]));
const framed = makeScene(validateCapture(outlier), 0, 1);
assert.equal(framed.radius, full.radius, 'Unbonded outliers do not change the bonded-model framing');
assert.equal(framed.atoms.length, 60, 'Outliers remain in the rendered model');
assert.equal(framed.atoms[52], 99.5, 'Framing translates all atoms without wrapping');
for (const [change, expected] of [
  [f => f.topology.atoms.splice(0), /atoms/],
  [f => f.topology.bonds[0][1] = 2, /index/],
  [f => f.topology.bonds[0][1] = .5, /index/],
  [f => f.topology.atoms[0][4] = '6', /element/],
  [f => f.frames[0][0][0] = NaN, /coordinates/],
  [f => f.frames[0][0][0] = '0', /coordinates/],
  [f => f.frames[0][0][0] = 1e7, /coordinates/],
  [f => f.frames[0][0].push(0), /coordinates/],
  [f => f.frames[0].pop(), /atom count/],
  [f => f.frames = [], /frames/]
]) {
  const invalid = structuredClone(fixture); change(invalid);
  assert.throws(() => validateCapture(invalid), expected);
}
assert.throws(() => parseCapture('{'), SyntaxError);
assert.throws(() => makeScene(capture, 5, 1), /index/);
assert.throws(() => makeScene(capture, 0, 4), /Style/);
assert.throws(() => viewProjection(0, 0, 1), /camera/);
for (const mesh of [sphereMesh(), cylinderMesh()]) {
  assert.equal(mesh.vertices.length % 6, 0);
  assert.equal(mesh.indices.length % 3, 0);
  assert.ok([...mesh.indices].every(i => i < mesh.vertices.length / 6));
  for (let i = 3; i < mesh.vertices.length; i += 6)
    assert.ok(Math.abs(Math.hypot(...mesh.vertices.slice(i, i + 3)) - 1) < 1e-6);
}
assert.equal(sphereMesh().vertices.length / 6, 17 * 13);
assert.equal(sphereMesh().indices.length, 16 * 11 * 6);
assert.equal(styleAt('cycle', 0), 1);
assert.equal(styleAt('cycle', 10), 2);
assert.equal(styleAt('cycle', 20), 3);
assert.equal(styleAt('cycle', 30), 1);
assert.equal(styleAt('random', 25), styleAt('random', 25));
assert.throws(() => styleAt('unknown', 0), /Style/);
for (const [fps, expectedDraws] of [[60, 121], [30, 61], [1, 3]]) {
  let due = 0, draws = 0;
  for (let frame = 0; frame <= 120; frame++) {
    const next = nextFrameDeadline(Math.round(frame * 1000 / 60), due, fps);
    if (next !== null) { due = next; draws++; }
  }
  assert.equal(draws, expectedDraws, `${fps} fps pacing with millisecond timestamps`);
}
assert.ok(nextFrameDeadline(1000, 0, 60) > 1000, 'Late callbacks skip overdue targets');
const firstTimestamp = 1000, newEpoch = 1000.75;
assert.notEqual(nextFrameDeadline(firstTimestamp, newEpoch, 60), null, 'Quantised first callback can meet the tolerance');
const firstSeconds = elapsedSeconds(firstTimestamp, newEpoch);
const firstLoopFrame = Math.floor(firstSeconds * 10) % capture.frames.length;
assert.equal(firstLoopFrame, 0, 'A first callback before the new epoch must select frame zero');
assert.doesNotThrow(() => makeScene(capture, firstLoopFrame, styleAt('cycle', firstSeconds)));
assert.equal(elapsedSeconds(newEpoch, newEpoch), 0);
assert.equal(elapsedSeconds(newEpoch + 10000, newEpoch), 10);

const savedFetch = globalThis.fetch;
try {
  globalThis.fetch = async () => new Response('missing', {status: 404});
  await assert.rejects(readCaptureURL('/missing'), /404/);
  globalThis.fetch = async () => new Response('{}', {headers: {'content-length': String(limits.bytes + 1)}});
  await assert.rejects(readCaptureURL('/oversized'), /32 MiB/);
  globalThis.fetch = async () => new Response(JSON.stringify(fixture));
  assert.equal((await readCaptureURL('/fixture')).atoms.length, 2);
} finally { globalThis.fetch = savedFetch; }

console.log('Browser capture, scene, mesh, camera and stream checks passed.');
