import resource, subprocess, sys
rc = subprocess.call(sys.argv[1:])
ru = resource.getrusage(resource.RUSAGE_CHILDREN)
with open('/out/rss-peak-kb.txt', 'w') as f:
    f.write(str(ru.ru_maxrss))
sys.exit(rc)
