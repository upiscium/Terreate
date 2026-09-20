root := justfile_directory()
script := root / ".automation/bin/agent_core.py"
lifecycle := root / ".automation/bin/task_lifecycle.py"
discard_pristine := root / ".automation/bin/discard_pristine.py"
init := root / ".automation/bin/init_context.py"

[working-directory: root]
preflight:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(init)}} preflight

[working-directory: root]
doctor:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(init)}} doctor

[working-directory: root]
context:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(init)}} context

[working-directory: root]
task-start task slug:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} start {{quote(task)}} {{quote(slug)}}

[working-directory: root]
task-start-from-issue issue slug:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} start-from-issue {{quote(issue)}} {{quote(slug)}}

[working-directory: root]
contract-check task='':
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} contract-check {{if task != '' { quote(task) } else { '' }}}

[working-directory: root]
contract-resume-check task='':
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} contract-resume-check {{if task != '' { quote(task) } else { '' }}}

[working-directory: root]
status task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} status {{quote(task)}}

[working-directory: root]
state-set task status:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} state-set {{quote(task)}} {{quote(status)}}

[working-directory: root]
batch-plan +tasks:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} batch-plan {{tasks}}

[working-directory: root]
work-unit-register task work_unit role objective:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} work-unit-register {{quote(task)}} {{quote(work_unit)}} {{quote(role)}} {{quote(objective)}}

[working-directory: root]
work-unit-next task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} work-unit-next {{quote(task)}}

[working-directory: root]
work-unit-create task role objective:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} work-unit-create {{quote(task)}} {{quote(role)}} {{quote(objective)}}

[working-directory: root]
work-unit-dispatch-check task work_unit role objective:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} work-unit-dispatch-check {{quote(task)}} {{quote(work_unit)}} {{quote(role)}} {{quote(objective)}}

[working-directory: root]
work-unit-status task work_unit:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} work-unit-status {{quote(task)}} {{quote(work_unit)}}

[working-directory: root]
work-unit-state-set task work_unit status evidence provider='' model='' error='':
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} work-unit-state-set {{quote(task)}} {{quote(work_unit)}} {{quote(status)}} {{quote(evidence)}} {{if provider != '' { '--provider ' + quote(provider) } else { '' }}} {{if model != '' { '--model ' + quote(model) } else { '' }}} {{if error != '' { '--error ' + quote(error) } else { '' }}}

[working-directory: root]
verify task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} agent verify {{quote(task)}}

[working-directory: root]
commit task message='':
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} agent commit {{quote(task)}} {{quote(message)}}

[working-directory: root]
push task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} agent push {{quote(task)}}

[working-directory: root]
pr-create task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} agent pr-create {{quote(task)}}

[working-directory: root]
pr-prepare task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} agent pr-prepare {{quote(task)}}

[working-directory: root]
pr-edit task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} agent pr-edit {{quote(task)}}

[working-directory: root]
pr-ready task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(script)}} agent pr-ready {{quote(task)}}

[working-directory: root]
discard-pristine task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(discard_pristine)}} {{quote(task)}}

[working-directory: root]
cleanup task:
    env PYTHONDONTWRITEBYTECODE=1 python3 -B {{quote(lifecycle)}} cleanup {{quote(task)}}
