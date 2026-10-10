globalThis.__intervals = Object.create(null);
globalThis.Mat4 = class Mat4 {
  constructor(value) {
    this.m = new Float32Array(value ? (value.m || value) : [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]);
    if (this.m.length !== 16) throw new TypeError('Mat4 requires 16 elements');
  }
  copy() { return new Mat4(this); }
};
globalThis.MediaPlaybackEvent = globalThis.MediaPlaybackEvent || {
  PLAYBACK_STOPPED: 0,
  PLAYBACK_PLAYING: 1,
  PLAYBACK_PAUSED: 2
};
