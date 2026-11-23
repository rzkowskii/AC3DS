// bzip2 decoder, written for this page from the published format description.
// Decodes one block at a time and hands the output out in chunks, so a large
// archive never has to exist unpacked in memory. Block and stream checksums
// are verified. Randomised blocks (written only by bzip2 before 0.9.5) are
// refused.
(function (root) {
    "use strict";

    const MAX_CODE_LENGTH = 20;
    const GROUP_RUN = 50;
    const CHUNK_SIZE = 1 << 20;

    const crcTable = new Int32Array(256);
    for (let i = 0; i < 256; i++) {
        let value = i << 24;
        for (let bit = 0; bit < 8; bit++) value = value < 0 ? (value << 1) ^ 0x04c11db7 : value << 1;
        crcTable[i] = value;
    }

    class Bzip2Error extends Error {}

    class Bzip2Decoder {
        constructor(input) {
            this.input = input;
            this.position = 0;      // next input byte to load into the bit buffer
            this.bitBuffer = 0;
            this.bitCount = 0;
            this.inStream = false;
            this.finished = false;
            this.blockSize = 0;
            this.streamCrc = 0;
            this.links = null;      // per block: low 8 bits byte, high 24 bits next index
            this.remaining = 0;     // symbols of the current block not yet output
            this.cursor = 0;
            this.previous = -1;
            this.runLength = 0;
            this.copies = 0;        // pending repeats of `previous` from a run count
            this.blockCrc = 0;
            this.expectedCrc = 0;
            this.blockOpen = false;
            this.chunk = new Uint8Array(CHUNK_SIZE);
            this.counts = new Int32Array(256);
            this.selectors = new Uint8Array(32768);
            this.groups = [];
            for (let i = 0; i < 6; i++) {
                this.groups.push({
                    lengths: new Uint8Array(258), limit: new Int32Array(MAX_CODE_LENGTH + 2),
                    base: new Int32Array(MAX_CODE_LENGTH + 2), symbols: new Uint16Array(258),
                    minLength: 0, maxLength: 0,
                });
            }
        }

        // Compressed bytes consumed so far, for progress reporting.
        get consumed() {
            return Math.min(this.position, this.input.length);
        }

        readBits(count) {
            let value = 0;
            while (count > 0) {
                if (this.bitCount === 0) {
                    if (this.position >= this.input.length) throw new Bzip2Error("The archive ends too early.");
                    this.bitBuffer = this.input[this.position++];
                    this.bitCount = 8;
                }
                const take = Math.min(count, this.bitCount);
                this.bitCount -= take;
                value = value * (1 << take) + ((this.bitBuffer >>> this.bitCount) & ((1 << take) - 1));
                count -= take;
            }
            return value;
        }

        // Returns the next chunk of unpacked bytes, or null at the end. The
        // chunk is reused by the following call.
        next() {
            const chunk = this.chunk;
            let filled = 0;
            while (filled < chunk.length) {
                if (this.remaining === 0 && this.copies === 0) {
                    if (this.blockOpen && !this.closeBlock()) {
                        throw new Bzip2Error("The archive is damaged (block checksum).");
                    }
                    if (this.finished || !this.openBlock()) break;
                }
                filled = this.drain(chunk, filled);
            }
            return filled ? chunk.subarray(0, filled) : null;
        }

        closeBlock() {
            const crc = ~this.blockCrc;
            this.blockOpen = false;
            this.streamCrc = ((this.streamCrc << 1) | (this.streamCrc >>> 31)) ^ crc;
            return crc === this.expectedCrc;
        }

        // Reads headers up to and including the next block. Returns false at
        // the end of the last stream.
        openBlock() {
            for (;;) {
                if (!this.inStream) {
                    this.bitCount = 0;
                    if (this.position >= this.input.length) {
                        this.finished = true;
                        return false;
                    }
                    if (this.readBits(8) !== 0x42 || this.readBits(8) !== 0x5a || this.readBits(8) !== 0x68) {
                        throw new Bzip2Error("This is not a bzip2 archive.");
                    }
                    const level = this.readBits(8) - 0x30;
                    if (level < 1 || level > 9) throw new Bzip2Error("This is not a bzip2 archive.");
                    this.blockSize = level * 100000;
                    if (!this.links || this.links.length < this.blockSize) this.links = new Uint32Array(this.blockSize);
                    this.streamCrc = 0;
                    this.inStream = true;
                }
                const high = this.readBits(24);
                const low = this.readBits(24);
                if (high === 0x314159 && low === 0x265359) {
                    this.readBlock();
                    return true;
                }
                if (high !== 0x177245 || low !== 0x385090) throw new Bzip2Error("The archive is damaged (block header).");
                if ((this.readBits(32) | 0) !== this.streamCrc) throw new Bzip2Error("The archive is damaged (checksum).");
                this.inStream = false;
            }
        }

        readBlock() {
            this.expectedCrc = this.readBits(32) | 0;
            if (this.readBits(1)) throw new Bzip2Error("The archive uses an obsolete bzip2 feature.");
            const origin = this.readBits(24);

            const order = new Uint8Array(256);   // move-to-front list of the bytes in use
            let used = 0;
            const ranges = this.readBits(16);
            for (let range = 0; range < 16; range++) {
                if (!(ranges & (0x8000 >> range))) continue;
                const present = this.readBits(16);
                for (let bit = 0; bit < 16; bit++) {
                    if (present & (0x8000 >> bit)) order[used++] = range * 16 + bit;
                }
            }
            if (used === 0) throw new Bzip2Error("The archive is damaged (symbol map).");
            const alphabet = used + 2;
            const endOfBlock = used + 1;

            const groupCount = this.readBits(3);
            const selectorCount = this.readBits(15);
            if (groupCount < 2 || groupCount > 6 || selectorCount < 1) {
                throw new Bzip2Error("The archive is damaged (coding tables).");
            }
            const selectors = this.selectors;
            const recent = [0, 1, 2, 3, 4, 5];
            for (let i = 0; i < selectorCount; i++) {
                let index = 0;
                while (this.readBits(1)) {
                    if (++index >= groupCount) throw new Bzip2Error("The archive is damaged (selectors).");
                }
                const group = recent[index];
                for (; index > 0; index--) recent[index] = recent[index - 1];
                recent[0] = group;
                selectors[i] = group;
            }
            for (let g = 0; g < groupCount; g++) this.readCodeLengths(this.groups[g], alphabet);

            // Huffman decoding with the move-to-front and zero-run stages
            // folded in. Locals keep the bit reader in registers.
            const input = this.input;
            const inputLength = input.length;
            const links = this.links;
            const blockSize = this.blockSize;
            const counts = this.counts.fill(0);
            let position = this.position;
            let bitBuffer = this.bitBuffer;
            let bitCount = this.bitCount;
            let count = 0;
            let run = 0;
            let runWeight = 1;
            let selector = 0;
            let groupLeft = 0;
            let limit = null;
            let base = null;
            let symbols = null;
            let minLength = 0;
            let maxLength = 0;
            for (;;) {
                if (groupLeft === 0) {
                    if (position > inputLength + 4) throw new Bzip2Error("The archive ends too early.");
                    if (selector >= selectorCount) throw new Bzip2Error("The archive is damaged (selectors).");
                    const group = this.groups[selectors[selector++]];
                    limit = group.limit;
                    base = group.base;
                    symbols = group.symbols;
                    minLength = group.minLength;
                    maxLength = group.maxLength;
                    groupLeft = GROUP_RUN;
                }
                groupLeft--;
                while (bitCount <= 24) {
                    bitBuffer = (bitBuffer << 8) | (position < inputLength ? input[position] : 0);
                    position++;
                    bitCount += 8;
                }
                let length = minLength;
                let code = (bitBuffer >>> (bitCount - length)) & ((1 << length) - 1);
                while (code > limit[length]) {
                    length++;
                    code = (bitBuffer >>> (bitCount - length)) & ((1 << length) - 1);
                }
                if (length > maxLength) throw new Bzip2Error("The archive is damaged (code).");
                bitCount -= length;
                const symbol = symbols[code - base[length]];

                if (symbol < 2) {
                    run += runWeight << symbol;
                    runWeight <<= 1;
                    if (run > blockSize) throw new Bzip2Error("The archive is damaged (run length).");
                    continue;
                }
                if (run) {
                    const byte = order[0];
                    if (count + run > blockSize) throw new Bzip2Error("The archive is damaged (block size).");
                    counts[byte] += run;
                    links.fill(byte, count, count + run);
                    count += run;
                    run = 0;
                    runWeight = 1;
                }
                if (symbol === endOfBlock) break;
                if (count >= blockSize) throw new Bzip2Error("The archive is damaged (block size).");
                const index = symbol - 1;
                const byte = order[index];
                if (index < 16) {
                    for (let i = index; i > 0; i--) order[i] = order[i - 1];
                } else {
                    order.copyWithin(1, 0, index);
                }
                order[0] = byte;
                counts[byte]++;
                links[count++] = byte;
            }
            // Give back whole bytes the bit buffer read ahead.
            position -= bitCount >> 3;
            bitBuffer >>>= bitCount & ~7;
            bitCount &= 7;
            if (position > inputLength) throw new Bzip2Error("The archive ends too early.");
            this.position = position;
            this.bitBuffer = bitBuffer;
            this.bitCount = bitCount;

            if (origin >= count) throw new Bzip2Error("The archive is damaged (block origin).");
            // Undo the Burrows-Wheeler transform: chain each byte to its successor.
            let sum = 0;
            for (let i = 0; i < 256; i++) {
                const total = counts[i];
                counts[i] = sum;
                sum += total;
            }
            for (let i = 0; i < count; i++) {
                const slot = counts[links[i] & 0xff]++;
                links[slot] |= i << 8;
            }
            this.cursor = links[origin] >>> 8;
            this.remaining = count;
            this.previous = -1;
            this.runLength = 0;
            this.copies = 0;
            this.blockCrc = -1;   // the CRC register starts as all ones
            this.blockOpen = true;
        }

        readCodeLengths(group, alphabet) {
            const lengths = group.lengths;
            let length = this.readBits(5);
            let minLength = MAX_CODE_LENGTH;
            let maxLength = 1;
            for (let symbol = 0; symbol < alphabet; symbol++) {
                for (;;) {
                    if (length < 1 || length > MAX_CODE_LENGTH) throw new Bzip2Error("The archive is damaged (code lengths).");
                    if (!this.readBits(1)) break;
                    length += this.readBits(1) ? -1 : 1;
                }
                lengths[symbol] = length;
                if (length < minLength) minLength = length;
                if (length > maxLength) maxLength = length;
            }
            // Canonical codes: for each length, the last code of that length
            // and the amount to subtract to index `symbols`.
            const limit = group.limit.fill(0x7fffffff);
            const base = group.base.fill(0);
            let code = 0;
            let assigned = 0;
            for (let size = minLength; size <= maxLength; size++) {
                base[size] = code - assigned;
                for (let symbol = 0; symbol < alphabet; symbol++) {
                    if (lengths[symbol] === size) {
                        group.symbols[assigned++] = symbol;
                        code++;
                    }
                }
                limit[size] = code - 1;
                code <<= 1;
            }
            group.minLength = minLength;
            group.maxLength = maxLength;
        }

        // Writes block output into chunk from `filled`, undoing the initial
        // run-length stage (four equal bytes are followed by a repeat count).
        drain(chunk, filled) {
            const links = this.links;
            const size = chunk.length;
            let remaining = this.remaining;
            let cursor = this.cursor;
            let previous = this.previous;
            let runLength = this.runLength;
            let copies = this.copies;
            let crc = this.blockCrc;
            while (filled < size) {
                if (copies > 0) {
                    const take = Math.min(copies, size - filled);
                    chunk.fill(previous, filled, filled + take);
                    for (let i = 0; i < take; i++) crc = (crc << 8) ^ crcTable[((crc >>> 24) ^ previous) & 0xff];
                    filled += take;
                    copies -= take;
                    continue;
                }
                if (remaining === 0) break;
                const entry = links[cursor];
                const byte = entry & 0xff;
                cursor = entry >>> 8;
                remaining--;
                if (runLength === 4) {
                    runLength = 0;
                    copies = byte;
                    continue;
                }
                if (byte === previous) {
                    runLength++;
                } else {
                    previous = byte;
                    runLength = 1;
                }
                chunk[filled++] = byte;
                crc = (crc << 8) ^ crcTable[((crc >>> 24) ^ byte) & 0xff];
            }
            this.remaining = remaining;
            this.cursor = cursor;
            this.previous = previous;
            this.runLength = runLength;
            this.copies = copies;
            this.blockCrc = crc;
            return filled;
        }
    }

    const api = { Bzip2Decoder, Bzip2Error };
    root.AC3DS = Object.assign(root.AC3DS || {}, api);
    if (typeof module === "object" && module.exports) module.exports = api;
})(typeof self !== "undefined" ? self : globalThis);
