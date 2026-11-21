-- Store the boundary cell at index zero and the dense data at 1..n.

local function next_value(x)
    local value = x * 3 + 1
    while value > 65535 do
        value = value - 65536
    end
    return value
end

local function merge_sort()
    local values = {[0] = 0}
    local scratch = {[0] = 0}
    local state = 17
    local i = 1
    while i < 4097 do
        state = next_value(state)
        values[#values + 1] = state
        scratch[#scratch + 1] = 0
        i = i + 1
    end
    local width = 1
    while width < 4096 do
        local left = 1
        while left < 4097 do
            local middle = left + width
            local stop = middle + width
            local a = left
            local b = middle
            local out = left
            while out < stop do
                local take_left = false
                if b == stop then
                    take_left = true
                else
                    if a < middle then
                        take_left = values[a] < values[b]
                    end
                end
                if take_left then
                    scratch[out] = values[a]
                    a = a + 1
                else
                    scratch[out] = values[b]
                    b = b + 1
                end
                out = out + 1
            end
            left = stop
        end
        local swap = values
        values = scratch
        scratch = swap
        width = width * 2
    end
    local checksum = 0
    i = 1
    while i < 4097 do
        checksum = checksum + i * values[i]
        i = i + 1
    end
    return checksum
end

local function sequence(seed)
    local values = {[0] = 0}
    local i = 1
    while i < 257 do
        seed = next_value(seed)
        values[#values + 1] = seed < 32768
        i = i + 1
    end
    return values
end

local function edit_distance()
    local a = sequence(17)
    local b = sequence(43)
    local previous = {[0] = 0}
    local current = {[0] = 0}
    local j = 1
    while j < 257 do
        previous[#previous + 1] = j
        current[#current + 1] = 0
        j = j + 1
    end
    local i = 1
    while i < 257 do
        current[0] = i
        j = 1
        while j < 257 do
            local best = previous[j - 1]
            if a[i] ~= b[j] then
                best = best + 1
            end
            local deletion = previous[j] + 1
            if deletion < best then
                best = deletion
            end
            local insertion = current[j - 1] + 1
            if insertion < best then
                best = insertion
            end
            current[j] = best
            j = j + 1
        end
        local swap = previous
        previous = current
        current = swap
        i = i + 1
    end
    return previous[256]
end

local function advance(p)
    p.x = p.x + p.vx
    p.y = p.y + p.vy
    if p.x > 255 then
        p.x = 510 - p.x
        p.vx = 0 - p.vx
    end
    if p.x < 0 then
        p.x = 0 - p.x
        p.vx = 0 - p.vx
    end
    if p.y > 255 then
        p.y = 510 - p.y
        p.vy = 0 - p.vy
    end
    if p.y < 0 then
        p.y = 0 - p.y
        p.vy = 0 - p.vy
    end
end

local function particles()
    local items = {[0] = 0}
    local i = 1
    while i < 257 do
        local p = {}
        p.x = i - 1
        p.y = 256 - i
        p.vx = 3
        p.vy = 2
        items[#items + 1] = p
        i = i + 1
    end
    local frame = 0
    while frame < 256 do
        i = 1
        while i < 257 do
            advance(items[i])
            i = i + 1
        end
        frame = frame + 1
    end
    local checksum = 0
    i = 1
    while i < 257 do
        local p = items[i]
        checksum = checksum + i * (p.x + 257 * p.y + 11 * p.vx + p.vy)
        i = i + 1
    end
    return checksum
end

local function measure(name, run)
    local start = os.clock()
    local result
    for i = 1, 4 do result = run() end
    local elapsed = os.clock() - start
    print(name)
    print(elapsed * 250)
    print(result)
end

for i = 1, 4 do
    measure("merge_sort", merge_sort)
    measure("edit_distance", edit_distance)
    measure("particles", particles)
end
