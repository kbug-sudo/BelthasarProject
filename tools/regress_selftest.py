r"""tools/regress.py's own logic, without running a check (the check regress-refresh-keeps-inputs-this-machine-lacks).

--refresh-input-hashes rebuilds every catalogued input's `used_by` from the checks: a file a check names only in its argv
or env (a state it loads) counts as read when this machine has it, or when the catalogue already holds it -- a state
another machine makes is still read there, and its entry must not be dropped here.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import regress  # noqa: E402

P = 'saves/_regress_selftest/absent.lakewram'
CHECKS = {'checks': [{'name': 'reads-it', 'argv': ['build/Belthasar.exe', '--loadwram', P]}]}


def main():
    if os.path.exists(os.path.join(regress.ROOT, P)):
        print('regress-selftest: FAIL %s exists here (the case needs a file this machine lacks)' % P)
        return 1
    kept = 'reads-it' in regress._input_users(CHECKS, {P: {'path': P}}).get(P, set())
    added = P in regress._input_users(CHECKS, {})
    ok = kept and not added
    print('regress-selftest: %s -- a catalogued input a check names in its argv, absent here: %s; an uncatalogued one: %s'
          % ('PASS' if ok else 'FAIL', 'kept' if kept else 'DROPPED', 'ADDED' if added else 'not added'))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
