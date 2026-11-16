#!/usr/bin/env python3
"""Add the Galaxy S9 jailbreak jobs to an iOS root's launchd.plist.

  jb-launchd.py MOUNTED_ROOT

This iOS boots with launchd_unsecure_cache=1: launchd reads its jobs from
/System/Library/xpc/launchd.plist only (nothing loads /Library/LaunchDaemons),
so the jobs a jailbreak would load at runtime go into that file:
  com.ios-s9.jbstart   starts jailbreakd from the Taurine basebinaries. NOT on
                       the critical path: libhooker ships its own injector
                       (/etc/rc.d/libhooker -> inject_criticald, libsyringe-14)
                       and its own /usr/lib/TweakInject.dylib. jailbreakd is
                       here because a real Taurine device runs it and some
                       packages ask it for a port; it is the piece that assumed
                       it had patched the kernel, so it may crash-loop.
  com.ios-s9.jbsetup   first boot: runs /prep_bootstrap.sh, dpkg -i the bundled
                       debs (Sileo, libhooker, safemode, PreferenceLoader,
                       NewTerm, OpenSSH), uicache, respring; then exits at once
                       on later boots
  com.ios-s9.s9net     publishes the USB Ethernet service (ue0) to configd
  com.openssh.sshd     the openssh-server package's own job (inetd style,
                       ports 22 and 2222)
  com.ios-s9.respring  one respring per boot once SpringBoard and libhooker's
                       injector are both up (the first SpringBoard starts
                       before injection is ready)
and SpringBoard gets DYLD_INSERT_LIBRARIES=/usr/lib/TweakInject.dylib
(libhooker's injector; the kernel's AMFI dyld policy allows it, and a missing
library is not fatal). Idempotent.
"""
import os
import plistlib
import sys

JOBS = {
    '/System/Library/LaunchDaemons/com.ios-s9.jbsetup.plist': {
        'Label': 'com.ios-s9.jbsetup',
        'ProgramArguments': ['/bin/sh', '/usr/libexec/ios-s9-jbsetup'],
        'RunAtLoad': True,
        'UserName': 'root',
        'POSIXSpawnType': 'Interactive',
    },
    '/System/Library/LaunchDaemons/com.ios-s9.s9net.plist': {
        'Label': 'com.ios-s9.s9net',
        'ProgramArguments': ['/usr/libexec/s9net'],
        'RunAtLoad': True,
        'KeepAlive': True,
        'ThrottleInterval': 10,
        'UserName': 'root',
        'StandardOutPath': '/private/var/ios-s9/s9net.log',
        'StandardErrorPath': '/private/var/ios-s9/s9net.log',
    },
    '/System/Library/LaunchDaemons/com.ios-s9.jbstart.plist': {
        'Label': 'com.ios-s9.jbstart',
        'ProgramArguments': ['/taurine/jailbreakd'],
        # RunAtLoad WITHOUT KeepAlive: on a device this slow, a crash-looping
        # daemon is worse than an absent one. If jailbreakd dies, the log says
        # so once and stays quiet; libhooker does not need it anyway.
        'RunAtLoad': True,
        'ThrottleInterval': 10,
        'UserName': 'root',
        'StandardOutPath': '/private/var/ios-s9/jailbreakd.log',
        'StandardErrorPath': '/private/var/ios-s9/jailbreakd.log',
    },
    '/System/Library/LaunchDaemons/com.ios-s9.respring.plist': {
        'Label': 'com.ios-s9.respring',
        'ProgramArguments': ['/bin/sh', '/usr/libexec/ios-s9-respring'],
        'RunAtLoad': True,
        'LaunchOnlyOnce': True,
        'UserName': 'root',
        'StandardOutPath': '/private/var/ios-s9/respring.log',
        'StandardErrorPath': '/private/var/ios-s9/respring.log',
    },
    '/System/Library/LaunchDaemons/com.openssh.sshd.plist': {
        'Label': 'com.openssh.sshd',
        'Program': '/bin/sh',
        'ProgramArguments': ['/bin/sh', '/usr/libexec/sshd-keygen-wrapper', '-i'],
        'SessionCreate': True,
        'Sockets': {
            'SSHListener': {'SockServiceName': 'ssh'},
            'SSHListener2': {'SockServiceName': '2222'},
        },
        'inetdCompatibility': {'Wait': False},
        'StandardErrorPath': '/dev/null',
        'POSIXSpawnType': 'Interactive',
        'UserName': 'root',
    },
}
INJECT = '/usr/lib/TweakInject.dylib'


def main():
    path = os.path.join(sys.argv[1], 'System/Library/xpc/launchd.plist')
    st = os.stat(path)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    daemons = plist['LaunchDaemons']
    for key, job in JOBS.items():
        daemons[key] = job
        print('job', job['Label'])
    found = 0
    for section in plist.values():
        if not isinstance(section, dict):
            continue
        for job in section.values():
            if isinstance(job, dict) and job.get('Label') == 'com.apple.SpringBoard':
                env = job.setdefault('EnvironmentVariables', {})
                env['DYLD_INSERT_LIBRARIES'] = INJECT
                found += 1
    if found != 1:
        sys.exit(f'SpringBoard job found {found} times, expected 1')
    print('SpringBoard: DYLD_INSERT_LIBRARIES =', INJECT)
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)


if __name__ == '__main__':
    main()
