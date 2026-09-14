// Undo and redo, by snapshot.
//
// A level is thirty kilobytes of JSON and an edit is a rare event driven by a
// person, so there is no reason for anything cleverer than keeping whole
// copies. The snapshot goes through the writer and the reader - the same code
// paths a save and a load use - which means an undone level is byte-identical
// to one loaded from the file, and that a bug in the writer shows up here
// rather than a week later in a diff.

import { emitJson } from './json_emit.js';
import { parseLevel, serializeLevel } from './level.js';

const LIMIT = 64;

export class History {
    constructor() {
        this.past = [];
        this.future = [];
        this.baseline = null;
    }

    // Call once after loading, before anything can be edited.
    reset(level) {
        this.past.length = 0;
        this.future.length = 0;
        this.baseline = this.snapshot(level);
    }

    snapshot(level) {
        return emitJson(serializeLevel(level));
    }

    // Record the state BEFORE an edit, so undo restores it. Anything the user
    // could redo is dropped, because they have just taken a different branch.
    record(level, label) {
        this.past.push({ text: this.snapshot(level), label });
        if (this.past.length > LIMIT) this.past.shift();
        this.future.length = 0;
    }

    canUndo() { return this.past.length > 0; }
    canRedo() { return this.future.length > 0; }

    undoLabel() { return this.past.at(-1)?.label ?? ''; }
    redoLabel() { return this.future.at(-1)?.label ?? ''; }

    undo(level) {
        if (!this.canUndo()) return null;
        const entry = this.past.pop();
        this.future.push({ text: this.snapshot(level), label: entry.label });
        return parseLevel(entry.text, level.path);
    }

    redo(level) {
        if (!this.canRedo()) return null;
        const entry = this.future.pop();
        this.past.push({ text: this.snapshot(level), label: entry.label });
        return parseLevel(entry.text, level.path);
    }

    // Whether anything has changed since the last load or save.
    isDirty(level) {
        return this.baseline !== null && this.snapshot(level) !== this.baseline;
    }

    markSaved(level) {
        this.baseline = this.snapshot(level);
    }
}
