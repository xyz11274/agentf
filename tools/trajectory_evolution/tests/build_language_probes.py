"""Compile optional language clients, then run the common live-server contract."""
import json
import os
from pathlib import Path
import subprocess
import sys

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
OUT = BUILD / "language-probes"
OUT.mkdir(parents=True, exist_ok=True)


def run(command, **kwargs):
    subprocess.run([str(value) for value in command], check=True, **kwargs)


run(["go", "build", "-o", OUT / "go-probe", SOURCE / "tests/clients/probe.go"], cwd=SOURCE / "clients/go")
run(["cargo", "build", "--locked", "--manifest-path", SOURCE / "clients/rust/Cargo.toml",
     "--example", "contract_probe", "--target-dir", OUT / "rust"])
run(["mvn", "-q", "-f", SOURCE / "clients/java/pom.xml", "dependency:build-classpath",
     "-Dmdep.outputFile=" + str(OUT / "java-classpath.txt")])
classpath = (OUT / "java-classpath.txt").read_text().strip()
run(["javac", "--release", "11", "-cp", classpath, "-d", OUT / "java",
     SOURCE / "clients/java/Evolution.java", SOURCE / "tests/clients/ClientProbe.java"])
run(["dotnet", "build", SOURCE / "tests/clients/csharp/ClientProbe.csproj", "-o", OUT / "csharp",
     "-p:BaseIntermediateOutputPath=" + str(OUT / "csharp-obj") + "/", "--nologo"])
commands = {
    "go": [str(OUT / "go-probe")],
    "rust": [str(OUT / "rust/debug/examples/contract_probe")],
    "java": ["java", "-cp", str(OUT / "java") + os.pathsep + classpath, "ClientProbe"],
    "csharp": ["dotnet", str(OUT / "csharp/ClientProbe.dll")],
}
run([sys.executable, SOURCE / "tests/test_language_clients.py", BUILD],
    env=dict(os.environ, EVOLUTION_LANGUAGE_PROBES=json.dumps(commands)))
