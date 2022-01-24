count = 0
fastest = 10000000
plus = 0
fastplus = 0
fastcount = 0
with open('res.log') as get:
    for line in get:
        count += 1
        if(line == '\n'):
            continue
        if(int(line) < fastest):
            fastest = int(line)
        if(int(line) < 23000):
            fastcount += 1
            fastplus += int(line)
        plus += int(line)
    print(count)
    print(fastest)
    print(plus/count)
    print(fastcount)
    print(fastplus/fastcount)
