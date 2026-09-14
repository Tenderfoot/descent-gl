// Just enough OBJ to draw the shipped models.
//
// The game loads these through tinyobjloader and cares about a great deal
// more than this does; all the editor needs is where a robot's silhouette is,
// so that placing one is not placing a grey box and hoping.
//
// Normals are ignored even where a file has them: the game computes flat
// per-face normals at load (`assets/models/*.obj` say so in their headers), so
// computing them here too is what matches.

// Returns { positions: Float32Array, count } wound the way the file is.
export function parseObj(text) {
    const vertices = [];
    const positions = [];

    for (const rawLine of text.split('\n')) {
        const line = rawLine.trim();
        if (line.length === 0 || line[0] === '#') continue;

        const parts = line.split(/\s+/);
        if (parts[0] === 'v') {
            vertices.push([Number(parts[1]), Number(parts[2]), Number(parts[3])]);
            continue;
        }
        if (parts[0] !== 'f') continue;

        // "1", "1/2", "1/2/3" and "1//3" all name the vertex first. Negative
        // indices count back from the end, which the format allows.
        const corners = parts.slice(1).map((token) => {
            const index = Number(token.split('/')[0]);
            return index > 0 ? index - 1 : vertices.length + index;
        });

        // A fan, so a quad or an n-gon comes out as triangles. The shipped
        // models are already triangles; this costs nothing and means a model
        // authored elsewhere still previews.
        for (let i = 1; i + 1 < corners.length; ++i) {
            for (const corner of [corners[0], corners[i], corners[i + 1]]) {
                const v = vertices[corner];
                if (!v) throw new Error(`face references vertex ${corner + 1}, which the file does not have`);
                positions.push(v[0], v[1], v[2]);
            }
        }
    }

    return { positions: new Float32Array(positions), count: positions.length / 3 };
}

// Fetches and caches every model once. The editor redraws the whole scene on
// each edit, so re-fetching per rebuild would be a request per object per
// keystroke.
export class ModelCache {
    constructor() {
        this.parsed = new Map();
        this.pending = new Map();
        this.missing = new Set();
    }

    get(type) {
        return this.parsed.get(type) ?? null;
    }

    // Returns a promise that resolves once the model is in the cache, or
    // immediately if it is already there or known to be absent. A level naming
    // a model that does not exist is the author's problem to see, not a reason
    // for the editor to stop drawing.
    load(type) {
        if (this.parsed.has(type) || this.missing.has(type)) return Promise.resolve();
        if (this.pending.has(type)) return this.pending.get(type);

        const request = fetch(`/api/models/${encodeURIComponent(type)}`)
            .then((response) => {
                if (!response.ok) throw new Error(`no model '${type}'`);
                return response.text();
            })
            .then((text) => { this.parsed.set(type, parseObj(text)); })
            .catch(() => { this.missing.add(type); })
            .finally(() => { this.pending.delete(type); });

        this.pending.set(type, request);
        return request;
    }

    loadAll(types) {
        return Promise.all([...new Set(types)].map((type) => this.load(type)));
    }
}
