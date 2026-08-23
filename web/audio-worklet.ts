// AudioWorklet processor: a FIFO of stereo s16 buffers pushed from the page (originating in the
// emulation worker). Underruns output silence.
/// <reference lib="dom" />

declare function registerProcessor(name: string, ctor: unknown): void;
declare const sampleRate: number;
declare class AudioWorkletProcessor {
  port: MessagePort;
  constructor();
}

class DmAudio extends AudioWorkletProcessor {
  queue: Int16Array[] = [];
  offset = 0; /* frame offset into queue[0] */
  queuedFrames = 0;
  srcRate = 48000;
  pos = 0; /* fractional source position for resampling to the context rate */
  /* Adaptive jitter buffer: prime `target` frames before playing; every underrun re-primes and
   * grows the target (up to 250 ms), quiet seconds shrink it slowly back toward 60 ms. */
  target = 48000 * 0.06;
  underruns = 0;
  blocks = 0;

  constructor() {
    super();
    this.port.onmessage = (e: MessageEvent) => {
      const d = e.data as { buf?: ArrayBuffer; rate?: number; ratio?: number };
      if (typeof d.ratio === "number" && d.ratio > 0) {
        this.targetRatio = Math.min(1.04, Math.max(0.7, d.ratio));
      }
      if (d.buf) {
        const arr = new Int16Array(d.buf);
        this.queue.push(arr);
        this.queuedFrames += arr.length / 2;
        /* cap latency well above the adaptive target */
        while (this.queuedFrames > this.target * 3 + this.srcRate / 10 && this.queue.length > 1) {
          const first = this.queue.shift()!;
          this.queuedFrames -= first.length / 2 - this.offset;
          this.offset = 0;
        }
      }
      if (d.rate) { this.srcRate = d.rate; this.target = d.rate * 0.06; }
    };
  }

  pull(): [number, number] {
    while (this.queue.length) {
      const first = this.queue[0];
      if (this.offset * 2 < first.length) {
        const l = first[this.offset * 2] / 32768;
        const r = first[this.offset * 2 + 1] / 32768;
        this.offset++;
        this.queuedFrames--;
        return [l, r];
      }
      this.queue.shift();
      this.offset = 0;
    }
    return [0, 0];
  }

  process(_inputs: Float32Array[][], outputs: Float32Array[][]): boolean {
    const out = outputs[0];
    // ~1 s telemetry so stutter reports carry numbers instead of adjectives
    if (++this.blocks >= Math.ceil(sampleRate / out[0].length)) {
      this.port.postMessage({ stats: {
        underruns: this.underruns,
        bufferedMs: this.queuedFrames / this.srcRate * 1000,
        targetMs: this.target / this.srcRate * 1000,
      } });
      if (this.underruns === 0) this.target = Math.max(this.srcRate * 0.06, this.target - this.srcRate * 0.004);
      this.underruns = 0;
      this.blocks = 0;
    }
    // Prime a cushion before starting and re-prime after an underrun: playing from the queue's
    // edge turns every producer hiccup into a click ("scratchy" audio). While silent, decay the
    // held sample so stopping doesn't click either.
    if (!this.started) {
      if (this.queuedFrames >= this.target) this.started = true;
      else {
        for (let i = 0; i < out[0].length; i++) {
          this.last[0] *= 0.94; this.last[1] *= 0.94;
          out[0][i] = this.last[0];
          if (out[1]) out[1][i] = this.last[1];
        }
        return true;
      }
    }
    // consume at the machine's real production rate (smoothed): a machine at 90% real time
    // then plays continuously at 90% pitch instead of starving every few hundred ms
    this.ratio += (this.targetRatio - this.ratio) * 0.05;
    const step = (this.srcRate * this.ratio) / sampleRate;
    for (let i = 0; i < out[0].length; i++) {
      this.pos += step;
      while (this.pos >= 1 && this.queuedFrames > 0) { this.pos -= 1; this.last = this.pull(); }
      if (this.pos >= 1) { // ran dry mid-block: grow the cushion and go back to priming
        this.started = false;
        this.pos = 0;
        this.underruns++;
        this.target = Math.min(this.srcRate * 0.25, this.target + this.srcRate * 0.03);
        for (; i < out[0].length; i++) {
          this.last[0] *= 0.94; this.last[1] *= 0.94;
          out[0][i] = this.last[0];
          if (out[1]) out[1][i] = this.last[1];
        }
        return true;
      }
      out[0][i] = this.last[0];
      if (out[1]) out[1][i] = this.last[1];
    }
    return true;
  }
  last: [number, number] = [0, 0];
  started = false;
  ratio = 1;
  targetRatio = 1;
}

registerProcessor("dm-audio", DmAudio);
