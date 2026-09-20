root := justfile_directory()
script := root / ".automation/bin/agent_core.py"

[working-directory: root]
check pr:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} integrate check {{quote(pr)}}

[working-directory: root]
merge pr:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} integrate merge {{quote(pr)}}

[working-directory: root]
finalize task pr:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} integrate finalize {{quote(task)}} {{quote(pr)}}
