use rustix::system::uname;

fn main() {
    let info = uname();
    println!(
        "pid {} on {} {} ({})",
        std::process::id(),
        info.sysname().to_string_lossy(),
        info.release().to_string_lossy(),
        info.machine().to_string_lossy(),
    );
}
