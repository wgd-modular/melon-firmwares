// A panel knob: drag up/down (or sideways), scroll, or use the arrow keys.
// Double-click returns it to its default.

const SVG = 'http://www.w3.org/2000/svg';
const SWEEP = 135;

function polar(r, degrees) {
  const a = degrees * Math.PI / 180;
  return [r * Math.sin(a), -r * Math.cos(a)];
}

function arc(r, from, to) {
  if (Math.abs(to - from) < 0.01) return '';
  const [x0, y0] = polar(r, Math.min(from, to));
  const [x1, y1] = polar(r, Math.max(from, to));
  const large = Math.abs(to - from) > 180 ? 1 : 0;
  return `M${x0.toFixed(2)} ${y0.toFixed(2)}A${r} ${r} 0 ${large} 1 ${x1.toFixed(2)} ${y1.toFixed(2)}`;
}

function element(name, attributes, parent) {
  const node = document.createElementNS(SVG, name);
  for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
  parent?.appendChild(node);
  return node;
}

export class Knob {
  constructor(slot, { label, value, defaultValue = value, origin = 0, format, onInput }) {
    this.value = value;
    this.defaultValue = defaultValue;
    this.origin = origin;
    this.format = format;
    this.onInput = onInput;
    this.colour = 'var(--melon)';

    const title = document.createElement('span');
    title.className = 'knob-label';
    title.textContent = label;

    this.node = document.createElement('div');
    this.node.className = 'knob';
    this.node.tabIndex = 0;
    this.node.setAttribute('role', 'slider');
    this.node.setAttribute('aria-label', label.charAt(0) + label.slice(1).toLowerCase());
    this.node.setAttribute('aria-valuemin', '0');
    this.node.setAttribute('aria-valuemax', '100');

    const svg = element('svg', { viewBox: '-62 -62 124 124', 'aria-hidden': 'true' }, this.node);
    element('circle', { r: 53, fill: 'none', stroke: '#f4f4f4', 'stroke-width': 2.4 }, svg);
    for (const angle of [-SWEEP, 0, SWEEP]) {
      const [x0, y0] = polar(53, angle);
      const [x1, y1] = polar(60, angle);
      element('line', { x1: x0, y1: y0, x2: x1, y2: y1, stroke: '#f4f4f4', 'stroke-width': 2.4, 'stroke-linecap': 'round' }, svg);
    }
    element('circle', { r: 47, fill: 'none', stroke: '#f4f4f4', 'stroke-width': 1.6 }, svg);
    element('circle', { r: 41.5, fill: 'none', stroke: '#8c8c8c', 'stroke-width': 9 }, svg);
    this.arc = element('path', { class: 'value-arc', fill: 'none', stroke: this.colour, 'stroke-width': 9 }, svg);
    this.cap = element('g', {}, svg);
    element('circle', { r: 31, fill: '#f4f4f4' }, this.cap);
    element('line', { x1: 0, y1: -9, x2: 0, y2: -26, stroke: '#000', 'stroke-width': 5, 'stroke-linecap': 'round' }, this.cap);

    this.readout = document.createElement('span');
    this.readout.className = 'knob-value';

    slot.append(title, this.node, this.readout);
    this.bind();
    this.draw();
  }

  set(value, notify = true) {
    const next = Math.min(1, Math.max(0, value));
    if (next === this.value && !notify) return;
    this.value = next;
    this.draw();
    if (notify) this.onInput(this.value);
  }

  setColour(colour) {
    this.colour = colour;
    this.arc.setAttribute('stroke', colour);
  }

  draw() {
    const angle = (this.value * 2 - 1) * SWEEP;
    const originAngle = (this.origin * 2 - 1) * SWEEP;
    this.cap.setAttribute('transform', `rotate(${angle.toFixed(2)})`);
    this.arc.setAttribute('d', arc(41.5, originAngle, angle));
    const text = this.format(this.value);
    this.readout.textContent = text;
    this.node.setAttribute('aria-valuenow', Math.round(this.value * 100));
    this.node.setAttribute('aria-valuetext', text);
  }

  bind() {
    let drag = null;
    this.node.addEventListener('pointerdown', event => {
      event.preventDefault();
      this.node.focus();
      this.node.setPointerCapture(event.pointerId);
      drag = { x: event.clientX, y: event.clientY, value: this.value };
    });
    this.node.addEventListener('pointermove', event => {
      if (!drag) return;
      const fine = event.shiftKey ? 0.2 : 1;
      const moved = (drag.y - event.clientY) + (event.clientX - drag.x);
      this.set(drag.value + moved / 220 * fine);
    });
    const end = () => { drag = null; };
    this.node.addEventListener('pointerup', end);
    this.node.addEventListener('pointercancel', end);
    this.node.addEventListener('dblclick', () => this.set(this.defaultValue));
    this.node.addEventListener('wheel', event => {
      event.preventDefault();
      this.set(this.value - Math.sign(event.deltaY) * (event.shiftKey ? 0.002 : 0.01) * Math.min(4, Math.abs(event.deltaY) / 25 + 1));
    }, { passive: false });
    this.node.addEventListener('keydown', event => {
      const steps = { ArrowUp: 0.01, ArrowRight: 0.01, ArrowDown: -0.01, ArrowLeft: -0.01, PageUp: 0.1, PageDown: -0.1 };
      if (event.key in steps) this.set(this.value + steps[event.key]);
      else if (event.key === 'Home') this.set(0);
      else if (event.key === 'End') this.set(1);
      else return;
      event.preventDefault();
    });
  }
}
