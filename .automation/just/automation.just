root := justfile_directory()
script := root / ".automation" / "bin" / "automation_upgrade.py"
maintenance := root / ".automation" / "bin" / "maintenance_lifecycle.py"

[working-directory: root]
version:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} version

[working-directory: root]
check-update source expected_revision="":
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} check-update --source {{quote(source)}} {{if expected_revision != "" { '--expected-source-revision ' + quote(expected_revision) } else { '' }}}

[working-directory: root]
upgrade source expected_revision:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} upgrade --source {{quote(source)}} --expected-source-revision {{quote(expected_revision)}}

[working-directory: root]
bootstrap-receipt source expected_revision:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} bootstrap-receipt --source {{quote(source)}} --expected-source-revision {{quote(expected_revision)}}

[working-directory: root]
rebind-maintenance-provenance source expected_revision:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} rebind-maintenance-provenance --source {{quote(source)}} --expected-source-revision {{quote(expected_revision)}}

[working-directory: root]
commit task message="":
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} commit {{quote(task)}} {{quote(message)}}

[working-directory: root]
maintenance-check task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(maintenance)}} check {{quote(task)}}

[working-directory: root]
maintenance-pr-create task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(maintenance)}} pr-create {{quote(task)}}

[working-directory: root]
maintenance-review-record task role evidence:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(maintenance)}} review-record {{quote(task)}} {{quote(role)}} {{quote(evidence)}}

[working-directory: root]
maintenance-finalize task pr:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(maintenance)}} finalize {{quote(task)}} {{quote(pr)}}
