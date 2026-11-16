(() => {
const limits = Object.freeze({bytes: 32 * 1024 * 1024, atoms: 200000, bonds: 1000000, frames: 10000, positions: 2000000});

const colours = [
  [.2, .2, .2], [.6, .6, .6], [32 / 255, 32 / 255, .8],
  [.8, 38 / 255, 38 / 255], [.6, .6, 38 / 255], [128 / 255, 0, .6]
];
const radii = [.17, .109, .155, .152, .18, .2];
const numberTypes = new Map([[6, 0], [1, 1], [3, 1], [7, 2], [10, 2], [8, 3], [16, 4]]);
const nameTypes = new Map([['C', 0], ['H', 1], ['N', 2], ['O', 3], ['S', 4]]);

function requireValue(condition, message) {
  if (!condition) throw new Error(message);
}

function parseCapture(text, policy = limits) {
  requireValue(typeof text === 'string', 'Capture must be JSON text');
  requireValue(new TextEncoder().encode(text).byteLength <= limits.bytes, 'Capture exceeds 32 MiB');
  return validateCapture(JSON.parse(text), policy);
}

function validateCapture(value, policy = limits) {
  requireValue(value && typeof value === 'object' && !Array.isArray(value), 'Capture must be an object');
  const topology = value.topology;
  requireValue(topology && typeof topology === 'object', 'Capture has no topology');
  requireValue(Array.isArray(topology.atoms) && topology.atoms.length > 0 && topology.atoms.length <= limits.atoms,
    'Topology must contain between 1 and 200000 atoms');
  const atoms = topology.atoms.map((atom, index) => {
    requireValue(Array.isArray(atom) && atom.length <= 16 && typeof atom[0] === 'string' && atom[0].length <= 256,
      `Atom ${index} must have a name`);
    let type = 5;
    if (atom.length > 4 && atom[4] !== null && atom[4] !== 0) {
      requireValue(Number.isInteger(atom[4]) && atom[4] > 0 && atom[4] <= 1000,
        `Atom ${index} has an invalid element number`);
      type = numberTypes.get(atom[4]) ?? 5;
    } else {
      for (const character of atom[0].toUpperCase()) {
        if (nameTypes.has(character)) { type = nameTypes.get(character); break; }
      }
    }
    return {radius: radii[type], colour: colours[type]};
  });
  requireValue(Array.isArray(topology.bonds) && topology.bonds.length <= limits.bonds,
    'Topology bonds must be an array with at most 1000000 entries');
  const bonds = topology.bonds.map((bond, index) => {
    requireValue(Array.isArray(bond) && bond.length === 2 &&
      bond.slice(0, 2).every(i => Number.isInteger(i) && i >= 0 && i < atoms.length),
    `Bond ${index} has an invalid atom index`);
    return [bond[0], bond[1]];
  });
  requireValue(Array.isArray(value.frames) && value.frames.length > 0 && value.frames.length <= policy.frames,
    `Capture must contain between 1 and ${policy.frames} frames`);
  const frames = validateFrames(value.frames, atoms.length, policy.positions);

  return {atoms, bonds, frames};
}


function validateFrames(value, atomCount, maximumPositions = limits.positions) {
  requireValue(Array.isArray(value), 'Frames must be an array');
  requireValue(Number.isInteger(atomCount) && atomCount > 0 && atomCount <= limits.atoms, 'Invalid atom count');
  requireValue(value.length <= Math.floor(maximumPositions / atomCount), 'Capture exceeds the decoded position budget');
  return value.map((frame, frameIndex) => {
    requireValue(Array.isArray(frame) && frame.length === atomCount, `Frame ${frameIndex} has the wrong atom count`);
    const positions = new Float32Array(frame.length * 3);
    frame.forEach((point, index) => {
      requireValue(Array.isArray(point) && point.length === 3 &&
        point.every(n => typeof n === 'number' && Number.isFinite(n) && Math.abs(n) <= 1e6),
      `Frame ${frameIndex}, atom ${index} has invalid coordinates`);
      positions.set(point, index * 3);
    });
    return positions;
  });
}

async function readCaptureURL(url) {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Capture request failed: HTTP ${response.status}`);
  const declaredSize = response.headers.get('content-length');
  if (declaredSize && Number(declaredSize) > limits.bytes) throw new Error('Capture exceeds 32 MiB');
  if (!response.body) throw new Error('Capture response has no readable body');
  const reader = response.body.getReader();
  const chunks = [];
  let bytes = 0;
  try {
    while (true) {
      const {done, value} = await reader.read();
      if (done) break;
      bytes += value.byteLength;
      if (bytes > limits.bytes) throw new Error('Capture exceeds 32 MiB');
      chunks.push(value);
    }
  } catch (error) {
    await reader.cancel();
    throw error;
  } finally { reader.releaseLock(); }
  const data = new Uint8Array(bytes);
  let offset = 0;
  for (const chunk of chunks) { data.set(chunk, offset); offset += chunk.byteLength; }
  return parseCapture(new TextDecoder('utf-8', {fatal: true}).decode(data));
}

globalThis.FAHCapture = {limits, parseCapture, validateCapture, validateFrames, readCaptureURL};
})();
