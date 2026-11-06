// A panel knob: drag up/down (or sideways), scroll, or use the arrow keys.
// Double-click returns it to its default. The rings and ticks are printed on
// the panel, so this only draws the cap, into a group of the panel's SVG that
// sits on the knob's hole. Sizes are in mm, like the panel.

const SVG = 'http://www.w3.org/2000/svg';
const SWEEP = 150;
const CAP = 3.757;

function element(name, attributes, parent) {
  const node = document.createElementNS(SVG, name);
  for (const [key, value] of Object.entries(attributes)) node.setAttribute(key, value);
  parent?.appendChild(node);
  return node;
}

export class Knob {
  constructor(node, { cap, label, value, defaultValue = value, format, readout, onInput }) {
    this.node = node;
    this.value = value;
    this.defaultValue = defaultValue;
    this.format = format;
    this.readout = readout;
    this.onInput = onInput;

    node.tabIndex = 0;
    node.setAttribute('role', 'slider');
    node.setAttribute('aria-label', label);
    node.setAttribute('aria-valuemin', '0');
    node.setAttribute('aria-valuemax', '100');

    this.cap = element('g', {}, cap);
    element('circle', { r: CAP, fill: '#fff' }, this.cap);
    element('circle', { r: CAP - 0.55, fill: 'none', stroke: '#e2e2e2', 'stroke-width': 0.18 }, this.cap);
    element('line', { x1: 0, y1: -1.05, x2: 0, y2: -3.05, stroke: '#000', 'stroke-width': 0.62, 'stroke-linecap': 'round' }, this.cap);

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

  draw() {
    const angle = (this.value * 2 - 1) * SWEEP;
    this.cap.setAttribute('transform', `rotate(${angle.toFixed(2)})`);
    const text = this.format(this.value);
    if (this.readout) this.readout.textContent = text;
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
