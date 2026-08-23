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

  constructor() {
    super();
    this.port.onmessage = (e: MessageEvent) => {
      const d = e.data as { buf?: ArrayBuffer; rate?: number };
      if (d.buf) {
        const arr = new Int16Array(d.buf);
        this.queue.push(arr);
        this.queuedFrames += arr.length / 2;
        /* cap latency: drop old data beyond ~250 ms */
        while (this.queuedFrames > this.srcRate / 4 && this.queue.length > 1) {
          const first = this.queue.shift()!;
          this.queuedFrames -= first.length / 2 - this.offset;
          this.offset = 0;
        }
      }
      if (d.rate) this.srcRate = d.rate;
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
    const step = this.srcRate / sampleRate;
    for (let i = 0; i < out[0].length; i++) {
      this.pos += step;
      let l = 0, r = 0;
      while (this.pos >= 1) { this.pos -= 1; [l, r] = this.last = this.pull(); }
      if (this.pos < 1 && this.last) { l = this.last[0]; r = this.last[1]; }
      out[0][i] = l;
      if (out[1]) out[1][i] = r;
    }
    return true;
  }
  last: [number, number] = [0, 0];
}

registerProcessor("dm-audio", DmAudio);
