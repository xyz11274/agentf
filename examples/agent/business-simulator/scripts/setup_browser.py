#!/usr/bin/env python3
"""Install the private browser/office runtime without modifying system Python."""
import argparse, os, shutil, subprocess, sys, tempfile
from pathlib import Path

PKG=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(PKG/'harness'))
from business_sim.browser import RUNTIME, executable

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--check',action='store_true',help='Only check an existing installation')
    args=parser.parse_args()
    if not args.check:
        subprocess.run([sys.executable,'-m','pip','install','--upgrade','--target',str(RUNTIME),'-r',str(PKG/'requirements-execution.txt')],check=True)
        if not executable():
            env={**os.environ,'PYTHONPATH':str(RUNTIME),'PLAYWRIGHT_BROWSERS_PATH':str(PKG/'outputs/browsers')}
            subprocess.run([sys.executable,'-m','playwright','install','chromium'],env=env,check=True)
    chrome=executable()
    if not chrome:raise SystemExit('Chromium is missing. Re-run without --check.')
    libs=PKG/'outputs/browser-libs';libs.mkdir(parents=True,exist_ok=True)
    env={**os.environ,'LD_LIBRARY_PATH':str(libs)+':'+os.environ.get('LD_LIBRARY_PATH','')}
    dependencies=subprocess.run(['ldd',chrome],env=env,capture_output=True,text=True,check=True).stdout
    if 'not found' in dependencies and not args.check and shutil.which('apt-get'):
        # Resolve package versions/checksums from the operator's configured apt repository.
        # Extract into this package only; no apt install, root files or system services.
        with tempfile.TemporaryDirectory(prefix='business-browser-libs-') as directory:
            subprocess.run(['apt-get','download','libgbm1','libwayland-server0'],cwd=directory,check=True)
            for package in Path(directory).glob('*.deb'):
                subprocess.run(['dpkg-deb','-x',str(package),directory+'/extract'],check=True)
            for library in Path(directory+'/extract').rglob('*.so*'):
                if library.is_file():shutil.copy2(library,libs/library.name)
    unresolved=[line.strip() for line in subprocess.run(['ldd',chrome],env=env,capture_output=True,text=True).stdout.splitlines() if 'not found' in line]
    if unresolved:raise SystemExit('Missing OS dependencies: '+', '.join(unresolved))
    # The same engine and configuration used by business tools must actually launch.
    from business_sim.browser import BrowserEngine
    from business_sim.state import Store,scope_key
    with tempfile.TemporaryDirectory(prefix='business-browser-check-') as directory:
        engine=BrowserEngine(Store(Path(directory)/'state.sqlite3'))
        try:
            result=engine.call(scope_key('setup-check','setup-check'),'service-manager',{'action':'browser_observe'})
            if result['status']!='succeeded':raise SystemExit(result)
        finally:engine.close()
    print('Chromium launch and private office/browser runtime verified.')

if __name__=='__main__':main()

