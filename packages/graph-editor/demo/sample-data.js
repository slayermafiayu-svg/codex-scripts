// Sample document: a camera push-in with a shaky X position, rotation and scale.
export function sampleDocument(fpsNum = 30, fpsDen = 1) {
  const F = (n) => (n * fpsDen) / fpsNum;
  return {
    fps: { num: fpsNum, den: fpsDen },
    range: { start: 0, end: 6 },
    channels: [
      {
        id: 'pos.x',
        name: 'Position X',
        shortName: 'X',
        group: 'Position',
        unit: 'px',
        color: '#ff5f6d',
        keys: [
          { id: 'px0', t: F(0), v: -320 },
          { id: 'px1', t: F(45), v: 80 },
          { id: 'px2', t: F(90), v: -40 },
          { id: 'px3', t: F(135), v: 220 },
          { id: 'px4', t: F(180), v: 0 },
        ],
      },
      {
        id: 'pos.y',
        name: 'Position Y',
        shortName: 'Y',
        group: 'Position',
        unit: 'px',
        color: '#5ddc7a',
        keys: [
          { id: 'py0', t: F(0), v: 120, interp: 'linear' },
          { id: 'py1', t: F(60), v: -60 },
          { id: 'py2', t: F(120), v: 40, interp: 'hold' },
          { id: 'py3', t: F(180), v: 0 },
        ],
      },
      {
        id: 'pos.z',
        name: 'Z Dist',
        shortName: 'Z',
        group: 'Position',
        unit: 'px',
        color: '#4fa3ff',
        keys: [
          { id: 'pz0', t: F(0), v: 1200, tangents: 'broken', out: { dt: F(30), dv: 0 } },
          { id: 'pz1', t: F(180), v: 600, tangents: 'broken', in: { dt: -F(70), dv: 0 } },
        ],
      },
      {
        id: 'rot.y',
        name: 'Rotation Y',
        shortName: 'Rot Y',
        group: 'Rotation',
        unit: '°',
        color: '#ffb547',
        keys: [
          { id: 'ry0', t: F(0), v: -15 },
          { id: 'ry1', t: F(90), v: 360 },
          { id: 'ry2', t: F(180), v: 720 },
        ],
      },
      {
        id: 'scale',
        name: 'Scale',
        shortName: 'Scale',
        group: 'Transform',
        unit: '%',
        color: '#c38bff',
        keys: [
          { id: 's0', t: F(-12), v: 60 },
          { id: 's1', t: F(30), v: 100 },
          { id: 's2', t: F(150), v: 100 },
          { id: 's3', t: F(192), v: 140 },
        ],
      },
      {
        id: 'opacity',
        name: 'Opacity',
        shortName: 'Opac',
        group: 'Material',
        unit: '%',
        color: '#3fd1c9',
        visible: false,
        keys: [
          { id: 'o0', t: F(0), v: 0 },
          { id: 'o1', t: F(20), v: 100 },
          { id: 'o2', t: F(160), v: 100 },
          { id: 'o3', t: F(180), v: 0 },
        ],
      },
    ],
  };
}
