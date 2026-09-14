// A JSON emitter that writes level files the way the shipped ones are written.
//
// JSON.stringify is not usable here for two reasons, both of which turn every
// save into an unreviewable diff:
//
//   * It prints an integral float as `7`, and the level files write `7.0`.
//     Which fields are floats is a schema question, not something the value
//     can answer once it is a JS number, so the emitter is told by key name.
//   * It puts short arrays on one line. The shipped files are nlohmann's
//     dump(2), which puts every array element on its own line.
//
// Key names whose numbers are indices or counts, never measurements. Everything
// else numeric is a float and gets a `.0` when it lands on a whole number.
const INTEGER_KEYS = new Set(['verts', 'portal', 'door', 'texture', 'segment', 'side']);

function emitNumber(value, isInteger) {
    if (!Number.isFinite(value)) throw new Error(`cannot write ${value} to a level file`);
    if (isInteger) return String(Math.round(value));
    // JSON.stringify gives the shortest representation that round-trips, which
    // is what nlohmann does too; it just omits the fractional part.
    const text = JSON.stringify(value);
    return Number.isInteger(value) && !/[.eE]/.test(text) ? `${text}.0` : text;
}

function emitValue(value, indent, isInteger) {
    const pad = '  '.repeat(indent);
    const inner = '  '.repeat(indent + 1);

    if (value === null) return 'null';
    if (typeof value === 'boolean') return String(value);
    if (typeof value === 'number') return emitNumber(value, isInteger);
    if (typeof value === 'string') return JSON.stringify(value);

    if (Array.isArray(value)) {
        if (value.length === 0) return '[]';
        const items = value.map((v) => inner + emitValue(v, indent + 1, isInteger));
        return `[\n${items.join(',\n')}\n${pad}]`;
    }

    const keys = Object.keys(value);
    if (keys.length === 0) return '{}';
    const items = keys.map(
        (k) => `${inner}${JSON.stringify(k)}: ${emitValue(value[k], indent + 1, INTEGER_KEYS.has(k))}`);
    return `{\n${items.join(',\n')}\n${pad}}`;
}

// Trailing newline included: saveLevel writes `dump(2) << '\n'`, and so does
// every other file in assets/.
export function emitJson(value) {
    return `${emitValue(value, 0, false)}\n`;
}
