import { render } from './dsp.js';

self.onmessage = ({ data: { id, samples, settings, quality } }) => {
  const result = render(samples, 0, samples.length, settings, quality);
  self.postMessage({ id, ...result }, [result.data.buffer, result.preview.buffer]);
};
