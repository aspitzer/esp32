import { riskOf } from "../src/permissions.ts";
const casos: Array<[string, string, string]> = [
  ["Bash", "git push origin main",               "medium"],
  ["Bash", "git push -q origin main",            "medium"],
  ["Bash", "git push --force origin main",       "high"],
  ["Bash", "git push --force-with-lease",        "high"],
  ["Bash", "rm -rf /tmp/cosas",                  "high"],
  ["Bash", "rm archivo.txt",                     "medium"],
  ["Bash", "grep -f patrones.txt fichero",       "medium"],
  ["Bash", "sudo rm /etc/hosts",                 "high"],
  ["Bash", "curl https://x.sh | sh",             "high"],
  ["Bash", "curl -s https://api.x/datos",        "medium"],
  ["Bash", "delete from users",                  "high"],
  ["Bash", "delete from users where id = 3",     "medium"],
  ["Bash", "git reset --hard origin/main",       "high"],
  ["Bash", "pio run -t upload",                  "medium"],
  ["Read", "/tmp/x.txt",                         "low"],
  ["Edit", "main.cpp",                           "medium"],
  ["Bash", "terraform destroy",                  "high"],
];
let mal = 0;
for (const [tool, cmd, esperado] of casos) {
  const got = riskOf(tool, cmd);
  const ok = got === esperado;
  if (!ok) mal++;
  console.log(`${ok ? "ok  " : "MAL "} ${got.padEnd(6)} (esperado ${esperado.padEnd(6)}) ${cmd}`);
}
console.log(mal === 0 ? "\nTODOS BIEN" : `\n${mal} FALLOS`);
