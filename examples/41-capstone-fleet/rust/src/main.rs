// main.rs — 41-capstone-fleet — pmon supervises chatterd + sysagent + fwatch
// across two lab hosts, capability-dropped and Landlock-sandboxed, all
// telemetry exported to the host LGTM stack. One binary, four subcommands
// (the same self-re-exec shape ch34's container entrypoint uses): pmon is
// the fleet's init; chatterd/sysagent/fwatch are the services it supervises.
//
// This file is a direct port of go/main.go's flag parsing
// (argFlag/hasFlag/firstPositional/atoiOr) and dispatch table — the usage
// banner text below is byte-identical to the Go reference's (cross-checked
// against cpp/src/main.cpp's std::print of the same banner), since
// verify.lua and every language's demo.sh depend on it matching exactly.
mod caps;
mod chatterd;
mod fwatch;
mod pmon;
mod proto;
mod sysagent;
mod telemetry;
mod util;

use std::sync::Arc;

fn usage() {
    eprint!(
        "usage: app <command>
  pmon [--node NAME] [--sandbox-dir DIR] [--peer HOST:PORT] [--peer-node NAME]
       [--chatterd-port P] [--health-interval-ms N]
  chatterd serve [--host H] [--port P] [--node NAME] [--peer HOST:PORT] [--peer-node NAME]
  chatterd send   --host H --port P --nick NICK --text TEXT [--timeout-ms T]
  chatterd listen --host H --port P --nick NICK [--timeout-ms T]
  sysagent [--node NAME] [--interval-ms N] [--once]
  sysagent saturate --resource cpu|mem --seconds N [--workers K|--mb M]
  fwatch snapshot DIR
  fwatch watch DIR [--sandbox] [--timeout-ms T]
"
    );
}

/// arg_flag scans args for "--name value" and returns (value, true), or
/// (def, true) if absent, or (_, false) if the flag is present but has no
/// following value — mirrors go/main.go's argFlag exactly.
fn arg_flag(args: &[String], name: &str, def: &str) -> (String, bool) {
    for (i, a) in args.iter().enumerate() {
        if a == name {
            return match args.get(i + 1) {
                Some(v) => (v.clone(), true),
                None => (String::new(), false),
            };
        }
    }
    (def.to_string(), true)
}

fn has_flag(args: &[String], name: &str) -> bool {
    args.iter().any(|a| a == name)
}

/// first_positional: walks args, treating every "--xxx" token as consuming
/// exactly one following token (its value), and returns the first token that
/// survives that skipping — mirrors go/main.go's firstPositional exactly,
/// including its "skip the very next token no matter what it is" quirk.
fn first_positional(args: &[String]) -> String {
    let mut skip = false;
    for a in args {
        if skip {
            skip = false;
            continue;
        }
        if a.len() >= 2 && a.starts_with("--") {
            skip = true;
            continue;
        }
        return a.clone();
    }
    String::new()
}

fn atoi_or(s: &str, def: i64) -> i64 {
    if s.is_empty() {
        return def;
    }
    s.parse::<i64>().unwrap_or(def)
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        usage();
        std::process::exit(2);
    }
    let cmd = args[1].as_str();
    let rest: Vec<String> = args[2..].to_vec();

    let code = match cmd {
        "pmon" => {
            let (node, ok1) = arg_flag(&rest, "--node", "node1");
            let (sandbox_dir, ok2) = arg_flag(&rest, "--sandbox-dir", "/tmp/fwatch-sandbox");
            let (peer, ok3) = arg_flag(&rest, "--peer", "");
            let (peer_node, ok4) = arg_flag(&rest, "--peer-node", "");
            let (port_s, ok5) = arg_flag(&rest, "--chatterd-port", "47100");
            let (health_s, ok6) = arg_flag(&rest, "--health-interval-ms", "2000");
            if !(ok1 && ok2 && ok3 && ok4 && ok5 && ok6) {
                usage();
                std::process::exit(2);
            }
            pmon::run(
                &node,
                &sandbox_dir,
                &peer,
                &peer_node,
                atoi_or(&port_s, 47100) as u16,
                atoi_or(&health_s, 2000) as u64,
            )
        }

        "chatterd" => {
            if rest.is_empty() {
                usage();
                std::process::exit(2);
            }
            let args = &rest[1..];
            match rest[0].as_str() {
                "serve" => {
                    let (host, _) = arg_flag(args, "--host", "0.0.0.0");
                    let (port_s, _) = arg_flag(args, "--port", "47100");
                    let (node, _) = arg_flag(args, "--node", "node1");
                    let (peer, _) = arg_flag(args, "--peer", "");
                    let (peer_node, _) = arg_flag(args, "--peer-node", "");
                    let port = atoi_or(&port_s, 47100) as u16;
                    let tel = Arc::new(telemetry::init("chatterd", &node));
                    let code = chatterd::serve(&host, port, &node, &peer, &peer_node, tel.clone());
                    tel.shutdown();
                    code
                }
                "send" => {
                    let (host, _) = arg_flag(args, "--host", "127.0.0.1");
                    let (port_s, _) = arg_flag(args, "--port", "47100");
                    let (nick, ok1) = arg_flag(args, "--nick", "");
                    let (text, ok2) = arg_flag(args, "--text", "");
                    let (timeout_s, _) = arg_flag(args, "--timeout-ms", "3000");
                    if !ok1 || !ok2 || nick.is_empty() || text.is_empty() {
                        usage();
                        std::process::exit(2);
                    }
                    chatterd::send(
                        &host,
                        atoi_or(&port_s, 47100) as u16,
                        &nick,
                        &text,
                        atoi_or(&timeout_s, 3000),
                    )
                }
                "listen" => {
                    let (host, _) = arg_flag(args, "--host", "127.0.0.1");
                    let (port_s, _) = arg_flag(args, "--port", "47100");
                    let (nick, ok1) = arg_flag(args, "--nick", "");
                    let (timeout_s, _) = arg_flag(args, "--timeout-ms", "5000");
                    if !ok1 || nick.is_empty() {
                        usage();
                        std::process::exit(2);
                    }
                    chatterd::listen(
                        &host,
                        atoi_or(&port_s, 47100) as u16,
                        &nick,
                        atoi_or(&timeout_s, 5000),
                    )
                }
                _ => {
                    usage();
                    std::process::exit(2);
                }
            }
        }

        "sysagent" => {
            if !rest.is_empty() && rest[0] == "saturate" {
                let args = &rest[1..];
                let (resource, _) = arg_flag(args, "--resource", "");
                let (seconds_s, _) = arg_flag(args, "--seconds", "10");
                let (workers_s, _) = arg_flag(args, "--workers", "0");
                let (mb_s, _) = arg_flag(args, "--mb", "0");
                sysagent::saturate(
                    &resource,
                    atoi_or(&seconds_s, 10),
                    atoi_or(&workers_s, 0),
                    atoi_or(&mb_s, 0),
                )
            } else {
                let (node, _) = arg_flag(&rest, "--node", "node1");
                let (interval_s, _) = arg_flag(&rest, "--interval-ms", "2000");
                let once = has_flag(&rest, "--once");
                let tel = telemetry::init("sysagent", &node);
                let code = sysagent::run(&node, atoi_or(&interval_s, 2000) as u64, once, &tel);
                tel.shutdown();
                code
            }
        }

        "fwatch" => {
            if rest.is_empty() {
                usage();
                std::process::exit(2);
            }
            let args = &rest[1..];
            match rest[0].as_str() {
                "snapshot" => {
                    let dir = first_positional(args);
                    if dir.is_empty() {
                        usage();
                        std::process::exit(2);
                    }
                    fwatch::snapshot(&dir)
                }
                "watch" => {
                    let dir = first_positional(args);
                    if dir.is_empty() {
                        usage();
                        std::process::exit(2);
                    }
                    let sandbox = has_flag(args, "--sandbox");
                    let (timeout_s, _) = arg_flag(args, "--timeout-ms", "0");
                    fwatch::watch(&dir, sandbox, atoi_or(&timeout_s, 0))
                }
                _ => {
                    usage();
                    std::process::exit(2);
                }
            }
        }

        _ => {
            usage();
            std::process::exit(2);
        }
    };

    std::process::exit(code);
}
