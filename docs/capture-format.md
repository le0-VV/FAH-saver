# Protein capture format

Capture files contain only topology and protein positions:

```json
{
  "topology": {
    "atoms": [["C", 0, 0, 0, 6], ["O", 0, 0, 0, 8]],
    "bonds": [[0, 1]]
  },
  "frames": [[[0, 0, 0], [0.2, 0, 0]]]
}
```

Atoms and frames use the current FAH viewer's arrays. Bond indices start at zero.
The optional atomic number is at atom index 4. A missing, null or zero number
uses the first recognised element letter in the name, as the FAH viewer does.
The six material groups are carbon, hydrogen, nitrogen, oxygen, sulphur and other.
The OpenMM compatibility values 3 and 10 map to hydrogen and nitrogen.

The writer preserves these six material presets and positions. It emits normal
element names instead of the original atom names. It rejects custom materials
because the shared model does not retain a general material schema. Captures
must not contain account details, machine addresses, client settings or logs.

The parser rejects malformed JSON, nesting deeper than 16 levels, more than four
million scalar values, files larger than 32 MiB, empty topology or empty frame
history. Additional limits are 200,000 atoms, 1,000,000 bonds, 10,000 frames and
2,000,000 positions across all frames. These are application limits, separate
from the upstream FAH visualisation data budget. Coordinates must be finite and
within -1,000,000 to 1,000,000. Each frame must have exactly one position per atom.

`tests/fixtures/synthetic-small.json` is invented test data. It is not a captured
WU and provides no evidence of live client compatibility or render performance.

## Rendering conventions

Meshes use a unit sphere and a unit-radius cylinder from y=0 to y=1. Triangle
winding faces outward. Vertex stride is 24 bytes; instance stride is 80 bytes.
Each instance contains a column-major 4x4 transform followed by an RGBA colour.
Cylinder transforms align their y axis with the bond. Coincident endpoints do
not produce a cylinder. The camera mean-centres coordinates. Its bounds include
the full space-filling atom radius in every style, so changing style does not
change the camera fit. Bond material is opaque grey with RGB 0.75.

`viewProjection` uses `P * T(0,0,-distance) * Ry(angle)`, with a right-handed camera
looking along -Z and +Y up. The field of view is 45 degrees and clip depth is 0..1.
Distance is `radius * 1.1 * max(1,1/aspect) / sin(pi/8)`. Near and far planes are
`max(radius*0.01,distance-radius*1.1)` and `distance+radius*1.1`. Vulkan needs a
viewport Y flip; WebGL needs depth conversion from 0..1 to -1..1.

Build and run the portable check from the repository root:

```sh
clang++ -std=c++17 -Iinclude -Ithird_party tests/core_tests.cpp src/core/Model.cpp -o /tmp/fah-core-tests
/tmp/fah-core-tests
```
