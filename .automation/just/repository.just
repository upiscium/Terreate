root := justfile_directory()
policy := root / ".automation/bin/repository_policy.py"

[working-directory: root]
policy-check:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(policy)}} check

[working-directory: root]
policy-apply:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(policy)}} apply
