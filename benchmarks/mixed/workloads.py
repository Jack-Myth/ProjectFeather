from time import perf_counter


class Particle:
    __slots__ = ("x", "y", "vx", "vy")

def next_value(x):
    value = x * 3 + 1
    while value > 65535:
        value = value - 65536
    return value


def merge_sort():
    values = [0]
    scratch = [0]
    state = 17
    i = 1
    while i < 4097:
        state = next_value(state)
        values.append(state)
        scratch.append(0)
        i = i + 1
    width = 1
    while width < 4096:
        left = 1
        while left < 4097:
            middle = left + width
            stop = middle + width
            a = left
            b = middle
            out = left
            while out < stop:
                take_left = False
                if b == stop:
                    take_left = True
                else:
                    if a < middle:
                        take_left = values[a] < values[b]
                if take_left:
                    scratch[out] = values[a]
                    a = a + 1
                else:
                    scratch[out] = values[b]
                    b = b + 1
                out = out + 1
            left = stop
        swap = values
        values = scratch
        scratch = swap
        width = width * 2
    checksum = 0
    i = 1
    while i < 4097:
        checksum = checksum + i * values[i]
        i = i + 1
    return checksum


def sequence(seed):
    values = [0]
    i = 1
    while i < 257:
        seed = next_value(seed)
        values.append(seed < 32768)
        i = i + 1
    return values


def edit_distance():
    a = sequence(17)
    b = sequence(43)
    previous = [0]
    current = [0]
    j = 1
    while j < 257:
        previous.append(j)
        current.append(0)
        j = j + 1
    i = 1
    while i < 257:
        current[0] = i
        j = 1
        while j < 257:
            best = previous[j - 1]
            if a[i] != b[j]:
                best = best + 1
            deletion = previous[j] + 1
            if deletion < best:
                best = deletion
            insertion = current[j - 1] + 1
            if insertion < best:
                best = insertion
            current[j] = best
            j = j + 1
        swap = previous
        previous = current
        current = swap
        i = i + 1
    return previous[256]


def advance(p):
    p.x = p.x + p.vx
    p.y = p.y + p.vy
    if p.x > 255:
        p.x = 510 - p.x
        p.vx = 0 - p.vx
    if p.x < 0:
        p.x = 0 - p.x
        p.vx = 0 - p.vx
    if p.y > 255:
        p.y = 510 - p.y
        p.vy = 0 - p.vy
    if p.y < 0:
        p.y = 0 - p.y
        p.vy = 0 - p.vy


def particles():
    items = [0]
    i = 1
    while i < 257:
        p = Particle()
        p.x = i - 1
        p.y = 256 - i
        p.vx = 3
        p.vy = 2
        items.append(p)
        i = i + 1
    frame = 0
    while frame < 256:
        i = 1
        while i < 257:
            advance(items[i])
            i = i + 1
        frame = frame + 1
    checksum = 0
    i = 1
    while i < 257:
        p = items[i]
        checksum = checksum + i * (p.x + 257 * p.y + 11 * p.vx + p.vy)
        i = i + 1
    return checksum


def measure(run):
    start = perf_counter()
    for _ in range(4):
        result = run()
    elapsed = perf_counter() - start
    print(run.__name__, elapsed * 250, result, sep="\n")


if __name__ == "__main__":
    for _ in range(4):
        for run in (merge_sort, edit_distance, particles):
            measure(run)
