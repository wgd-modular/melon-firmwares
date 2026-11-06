// Plays the rendered recording the way radio.ino does: a trigger starts at the
// POSITION point, forward runs to the end, reverse runs from the end back to
// the POSITION point, and loop repeats that stretch.

export class Player {
  constructor(onChange) {
    this.onChange = onChange;
    this.context = null;
    this.samples = null;
    this.rate = 1;
    this.length = 0;
    this.speed = 1;
    this.reversed = false;
    this.loop = false;
    this.startPos = 0;
    this.node = null;
    this.anchor = null;
    this.buffers = null;
  }

  ensureContext() {
    if (!this.context) {
      this.context = new AudioContext();
      this.analyser = this.context.createAnalyser();
      this.analyser.fftSize = 1024;
      this.analyser.connect(this.context.destination);
      this.scope = new Float32Array(this.analyser.fftSize);
    }
    if (this.context.state === 'suspended') this.context.resume();
  }

  get playing() {
    return this.node !== null;
  }

  setAudio(samples, rate) {
    const position = this.position();
    const ratio = this.length ? samples.length / this.length : 1;
    this.samples = samples;
    this.rate = rate;
    this.length = samples.length;
    this.buffers = null;
    this.startPos = Math.min(Math.floor(this.startPos * ratio), Math.max(0, this.length - 1));
    if (position !== null) this.play(Math.min(this.length - 1, Math.floor(position * ratio)));
  }

  makeBuffers() {
    if (this.buffers || !this.samples) return;
    const forward = this.context.createBuffer(1, this.length, this.rate);
    forward.copyToChannel(this.samples, 0);
    const reversed = new Float32Array(this.length);
    for (let i = 0; i < this.length; i++) reversed[i] = this.samples[this.length - 1 - i];
    const backward = this.context.createBuffer(1, this.length, this.rate);
    backward.copyToChannel(reversed, 0);
    this.buffers = { forward, backward };
  }

  trigger(percent) {
    if (!this.samples || this.length < 2) return;
    this.ensureContext();
    this.startPos = Math.floor(this.length * percent / 100);
    this.play(this.reversed ? this.length - 1 : this.startPos);
  }

  play(position) {
    this.halt();
    this.makeBuffers();
    const node = this.context.createBufferSource();
    node.playbackRate.value = this.speed;
    node.connect(this.analyser);

    const { rate, length, startPos } = this;
    if (!this.reversed) {
      node.buffer = this.buffers.forward;
      if (this.loop) {
        node.loop = true;
        node.loopStart = startPos / rate;
        node.loopEnd = length / rate;
      }
      node.start(0, Math.max(position, 0) / rate);
    } else {
      node.buffer = this.buffers.backward;
      const span = length - startPos;
      const offset = Math.max(0, length - 1 - position);
      if (this.loop) {
        node.loop = true;
        node.loopStart = 0;
        node.loopEnd = span / rate;
        node.start(0, offset / rate);
      } else {
        node.start(0, offset / rate, Math.max(0, span - offset) / rate);
      }
    }

    node.onended = () => {
      if (this.node !== node) return;
      this.node = null;
      this.anchor = null;
      this.onChange();
    };
    this.node = node;
    this.anchor = { time: this.context.currentTime, position };
    this.onChange();
  }

  halt() {
    if (!this.node) return;
    const node = this.node;
    this.node = null;
    this.anchor = null;
    node.onended = null;
    try { node.stop(); } catch { /* already stopped */ }
    node.disconnect();
  }

  stop() {
    this.halt();
    this.onChange();
  }

  // Current sample index in the rendered recording, or null when silent.
  position() {
    if (!this.anchor || !this.context) return null;
    const moved = (this.context.currentTime - this.anchor.time) * this.rate * this.speed;
    const { length, startPos } = this;
    if (!this.reversed) {
      let p = this.anchor.position + moved;
      if (p >= length) {
        if (!this.loop) return null;
        p = startPos + ((p - startPos) % Math.max(1, length - startPos));
      }
      return p;
    }
    const span = length - startPos;
    let r = length - 1 - this.anchor.position + moved;
    if (r >= span) {
      if (!this.loop) return null;
      r %= Math.max(1, span);
    }
    return length - 1 - r;
  }

  setSpeed(speed) {
    const position = this.position();
    this.speed = speed;
    if (this.node && position !== null) {
      this.node.playbackRate.value = speed;
      this.anchor = { time: this.context.currentTime, position };
    }
  }

  setReversed(reversed) {
    if (reversed === this.reversed) return;
    const position = this.position();
    this.reversed = reversed;
    if (position !== null) this.play(Math.floor(position));
  }

  setLoop(loop) {
    if (loop === this.loop) return;
    const position = this.position();
    this.loop = loop;
    if (position !== null) this.play(Math.floor(position));
  }

  level() {
    if (!this.analyser || !this.node) return 0;
    this.analyser.getFloatTimeDomainData(this.scope);
    let sum = 0;
    for (const s of this.scope) sum += s * s;
    return Math.min(1, Math.sqrt(sum / this.scope.length) * 3);
  }
}
