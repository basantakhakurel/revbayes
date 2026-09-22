run_integration_tests_sh = find_program('./run_integration_tests.sh')

test('run_integration_tests',
     run_integration_tests_sh,
     timeout: 3000,
     workdir: meson.project_source_root() / 'tests',
     args: [rb.full_path()])

# Conformance tests for `rb --server` (protocol used by graphical front ends). Standard-library Python only.
python3 = find_program('python3', required: false)
if python3.found()
  test('server_conformance',
       python3,
       timeout: 300,
       suite: 'server',
       args: [files('server/run_conformance.py'), rb.full_path()])
endif

