//! In-process mock SSH provider.
//!
//! Simulates the externally observable behaviour of a real SSH session — a
//! connect delay, a login banner, command echoing with a fake prompt, and an
//! error path — without any network access. It is the default backend for the
//! framework stage and is what the demo UI exercises.
//!
//! Error path: any host whose name starts with `"fail"` is treated as
//! unreachable, so the UI can demonstrate the failure state.

use crate::fs::{looks_like_text, FileEntry, FileKind, FileProvider};
use crate::provider::{ConnectionConfig, ProviderEvent, ProviderSink, SshProvider};
use crate::secret::Secret;
use crate::{CoreError, CoreResult, RsErrorCode};
use async_trait::async_trait;
use std::collections::{HashMap, HashSet};
use std::time::Duration;

/// Home directory of the simulated filesystem.
const MOCK_HOME: &str = "/home/researcher";

/// In-memory filesystem backing [`MockFileProvider`]. Lets the file tree / editor
/// UI be exercised without a real SFTP server.
struct MockFs {
    dirs: HashSet<String>,
    files: HashMap<String, Vec<u8>>,
}

impl MockFs {
    fn seeded() -> Self {
        let mut dirs = HashSet::new();
        let mut files = HashMap::new();
        for d in [
            MOCK_HOME,
            "/home/researcher/data",
            "/home/researcher/experiments",
        ] {
            dirs.insert(d.to_string());
        }
        files.insert(
            "/home/researcher/train.py".into(),
            b"import torch\n\ndef main():\n    print('training on the mock cluster')\n\nif __name__ == '__main__':\n    main()\n".to_vec(),
        );
        files.insert(
            "/home/researcher/kernel.cpp".into(),
            b"#include <cstdio>\nint main(){ printf(\"hello from mock\\n\"); return 0; }\n"
                .to_vec(),
        );
        files.insert(
            "/home/researcher/README.md".into(),
            b"# Mock workspace\n\nDemo files served by the in-memory mock provider.\n".to_vec(),
        );
        files.insert(
            "/home/researcher/results.csv".into(),
            b"epoch,loss\n1,0.42\n2,0.31\n".to_vec(),
        );
        files.insert("/home/researcher/model.bin".into(), vec![0u8; 2048]);
        files.insert(
            "/home/researcher/data/run.sh".into(),
            b"#!/bin/sh\npython ../train.py\n".to_vec(),
        );
        Self { dirs, files }
    }

    fn resolve<'a>(&self, path: &'a str) -> &'a str {
        if path.is_empty() || path == "." {
            MOCK_HOME
        } else {
            path
        }
    }

    fn entry(path: &str, name: &str, is_dir: bool, size: u64) -> FileEntry {
        FileEntry {
            name: name.to_string(),
            path: path.to_string(),
            kind: if is_dir {
                FileKind::Directory
            } else {
                FileKind::File
            },
            size,
            modified_unix: -1,
            mode: if is_dir { 0o755 } else { 0o644 },
            editable_text: !is_dir && looks_like_text(name),
        }
    }

    fn children(&self, dir: &str) -> Vec<FileEntry> {
        let prefix = format!("{}/", dir.trim_end_matches('/'));
        let mut out = Vec::new();
        for d in &self.dirs {
            if let Some(rest) = d.strip_prefix(&prefix) {
                if !rest.is_empty() && !rest.contains('/') {
                    out.push(Self::entry(d, rest, true, 0));
                }
            }
        }
        for (p, content) in &self.files {
            if let Some(rest) = p.strip_prefix(&prefix) {
                if !rest.is_empty() && !rest.contains('/') {
                    out.push(Self::entry(p, rest, false, content.len() as u64));
                }
            }
        }
        out.sort_by(|a, b| {
            (b.kind == FileKind::Directory)
                .cmp(&(a.kind == FileKind::Directory))
                .then(a.name.cmp(&b.name))
        });
        out
    }
}

/// In-memory [`FileProvider`] used by the mock backend.
pub struct MockFileProvider {
    fs: MockFs,
}

impl MockFileProvider {
    fn new() -> Self {
        Self {
            fs: MockFs::seeded(),
        }
    }
}

fn not_found(path: &str) -> CoreError {
    CoreError::with_detail(RsErrorCode::Internal, format!("no such file: {path}"))
}

#[async_trait]
impl FileProvider for MockFileProvider {
    async fn list_dir(&mut self, path: &str) -> CoreResult<Vec<FileEntry>> {
        let dir = self.fs.resolve(path).to_string();
        if !self.fs.dirs.contains(&dir) {
            return Err(not_found(&dir));
        }
        Ok(self.fs.children(&dir))
    }

    async fn stat(&mut self, path: &str) -> CoreResult<FileEntry> {
        let p = self.fs.resolve(path).to_string();
        let name = p.rsplit('/').next().unwrap_or(&p).to_string();
        if self.fs.dirs.contains(&p) {
            Ok(MockFs::entry(&p, &name, true, 0))
        } else if let Some(c) = self.fs.files.get(&p) {
            Ok(MockFs::entry(&p, &name, false, c.len() as u64))
        } else {
            Err(not_found(&p))
        }
    }

    async fn read_file(&mut self, path: &str, max_len: u64) -> CoreResult<Vec<u8>> {
        let mut data = self
            .fs
            .files
            .get(path)
            .cloned()
            .ok_or_else(|| not_found(path))?;
        if max_len > 0 && data.len() as u64 > max_len {
            data.truncate(max_len as usize);
        }
        Ok(data)
    }

    async fn write_file(&mut self, path: &str, data: &[u8]) -> CoreResult<()> {
        self.fs.files.insert(path.to_string(), data.to_vec());
        Ok(())
    }

    async fn rename(&mut self, from: &str, to: &str) -> CoreResult<()> {
        if let Some(content) = self.fs.files.remove(from) {
            self.fs.files.insert(to.to_string(), content);
            return Ok(());
        }
        if !self.fs.dirs.contains(from) {
            return Err(not_found(from));
        }

        let from_prefix = format!("{}/", from.trim_end_matches('/'));
        let to_prefix = format!("{}/", to.trim_end_matches('/'));
        let moved_dirs: Vec<String> = self
            .fs
            .dirs
            .iter()
            .filter(|d| d.as_str() == from || d.starts_with(&from_prefix))
            .cloned()
            .collect();
        for dir in &moved_dirs {
            self.fs.dirs.remove(dir);
        }
        for dir in moved_dirs {
            let renamed = if dir == from {
                to.to_string()
            } else {
                dir.replacen(&from_prefix, &to_prefix, 1)
            };
            self.fs.dirs.insert(renamed);
        }

        let moved_files: Vec<(String, Vec<u8>)> = self
            .fs
            .files
            .iter()
            .filter(|(p, _)| p.starts_with(&from_prefix))
            .map(|(p, content)| (p.clone(), content.clone()))
            .collect();
        for (file, _) in &moved_files {
            self.fs.files.remove(file);
        }
        for (file, content) in moved_files {
            self.fs
                .files
                .insert(file.replacen(&from_prefix, &to_prefix, 1), content);
        }
        Ok(())
    }

    async fn remove(&mut self, path: &str, recursive: bool) -> CoreResult<()> {
        if self.fs.files.remove(path).is_some() {
            return Ok(());
        }
        if self.fs.dirs.contains(path) {
            let prefix = format!("{}/", path.trim_end_matches('/'));
            let has_children = self.fs.dirs.iter().any(|d| d.starts_with(&prefix))
                || self.fs.files.keys().any(|f| f.starts_with(&prefix));
            if has_children && !recursive {
                return Err(CoreError::with_detail(
                    RsErrorCode::InvalidState,
                    "directory not empty",
                ));
            }
            self.fs
                .dirs
                .retain(|d| d != path && !d.starts_with(&prefix));
            self.fs.files.retain(|f, _| !f.starts_with(&prefix));
            Ok(())
        } else {
            Err(not_found(path))
        }
    }

    async fn mkdir(&mut self, path: &str) -> CoreResult<()> {
        self.fs.dirs.insert(path.to_string());
        Ok(())
    }

    async fn copy(&mut self, from: &str, to: &str) -> CoreResult<()> {
        if let Some(content) = self.fs.files.get(from).cloned() {
            self.fs.files.insert(to.to_string(), content);
            return Ok(());
        }
        if !self.fs.dirs.contains(from) {
            return Err(not_found(from));
        }

        let from_prefix = format!("{}/", from.trim_end_matches('/'));
        let to_prefix = format!("{}/", to.trim_end_matches('/'));
        let copied_dirs: Vec<String> = self
            .fs
            .dirs
            .iter()
            .filter(|d| d.as_str() == from || d.starts_with(&from_prefix))
            .cloned()
            .collect();
        for dir in copied_dirs {
            let copied = if dir == from {
                to.to_string()
            } else {
                dir.replacen(&from_prefix, &to_prefix, 1)
            };
            self.fs.dirs.insert(copied);
        }

        let copied_files: Vec<(String, Vec<u8>)> = self
            .fs
            .files
            .iter()
            .filter(|(p, _)| p.starts_with(&from_prefix))
            .map(|(p, content)| (p.clone(), content.clone()))
            .collect();
        for (file, content) in copied_files {
            self.fs
                .files
                .insert(file.replacen(&from_prefix, &to_prefix, 1), content);
        }
        Ok(())
    }
}

/// Simulated SSH provider. See module docs.
pub struct MockProvider {
    config: ConnectionConfig,
    sink: Option<ProviderSink>,
    connected: bool,
    file_provider: Option<Box<dyn FileProvider>>,
}

impl MockProvider {
    /// Create a mock provider for the given connection config.
    pub fn new(config: ConnectionConfig) -> Self {
        Self {
            config,
            sink: None,
            connected: false,
            file_provider: Some(Box::new(MockFileProvider::new())),
        }
    }

    fn prompt(&self) -> String {
        format!("{}@{}:~$ ", self.config.username, self.config.host)
    }
}

#[async_trait]
impl SshProvider for MockProvider {
    fn set_secret(&mut self, _secret: Secret) {
        // The mock does not authenticate; the secret is dropped (and zeroized).
    }

    fn take_file_provider(&mut self) -> Option<Box<dyn FileProvider>> {
        self.file_provider.take()
    }

    async fn connect(&mut self, sink: ProviderSink) -> CoreResult<()> {
        // Simulate connection latency (also gives `cancel` something to interrupt).
        tokio::time::sleep(Duration::from_millis(200)).await;

        if self.config.host.starts_with("fail") || self.config.host.is_empty() {
            return Err(CoreError::with_detail(
                RsErrorCode::ConnectFailed,
                format!("模拟：主机 “{}” 无法连接", self.config.host),
            ));
        }

        self.connected = true;
        self.sink = Some(sink.clone());

        // Emit a login banner as bulk terminal data.
        let banner = format!(
            "ResearchSSH-Next 模拟 SSH 服务器\r\n\
             已连接到 {}:{}(用户 {})\r\n\
             (模拟 Provider,尚未接入真实 SSH)\r\n{}",
            self.config.host,
            self.config.port,
            self.config.username,
            self.prompt()
        );
        sink.emit(ProviderEvent::Data(banner.into_bytes()));
        Ok(())
    }

    async fn send(&mut self, data: &[u8]) -> CoreResult<()> {
        if !self.connected {
            return Err(CoreError::new(RsErrorCode::NotConnected));
        }
        let sink = self
            .sink
            .clone()
            .ok_or_else(|| CoreError::new(RsErrorCode::InvalidState))?;

        let command = String::from_utf8_lossy(data);
        let trimmed = command.trim_end_matches(['\r', '\n']);

        // Echo the command, then a canned response, then a fresh prompt — all as
        // one bulk write.
        let mut out = String::new();
        out.push_str(trimmed);
        out.push_str("\r\n");
        out.push_str(&mock_response(trimmed));
        out.push_str(&self.prompt());

        // Tiny delay to mimic round-trip latency.
        tokio::time::sleep(Duration::from_millis(50)).await;
        sink.emit(ProviderEvent::Data(out.into_bytes()));
        Ok(())
    }

    async fn disconnect(&mut self) -> CoreResult<()> {
        self.connected = false;
        self.sink = None;
        Ok(())
    }
}

/// Canned responses for a few research-flavoured commands.
fn mock_response(cmd: &str) -> String {
    let line = match cmd {
        "" => String::new(),
        c if c.contains('\u{3}') => "^C\r\n".to_string(),
        c if c.starts_with("nvidia-smi") => "\
GPU 0: NVIDIA A100-SXM4-80GB  | 42C |  78W / 400W | 12345MiB / 81920MiB | 37%\r\n\
GPU 1: NVIDIA A100-SXM4-80GB  | 39C |  61W / 400W |  2048MiB / 81920MiB |  5%\r\n"
            .to_string(),
        c if c.starts_with("squeue") => "\
  JOBID PARTITION     NAME     USER ST       TIME  NODES NODELIST\r\n\
 102934       gpu  train.sh  alice  R    2:13:05      4 gpu[01-04]\r\n"
            .to_string(),
        c if c.starts_with("df") => "\
Filesystem      Size  Used Avail Use% Mounted on\r\n\
/dev/nvme0n1    1.8T  0.9T  0.8T  53% /home\r\n\
scratch          50T   31T   19T  62% /scratch\r\n"
            .to_string(),
        c if c.starts_with("python") => "Python 3.11.8(模拟)\r\n".to_string(),
        c if c.contains("__RSSH_RESOURCE_BEGIN__") => "\
__RSSH_RESOURCE_BEGIN__\r\n\
0, NVIDIA A100-SXM4-80GB, 37, 12345, 81920\r\n\
1, NVIDIA A100-SXM4-80GB, 5, 2048, 81920\r\n\
__RSSH_RESOURCE_PROCESSES__\r\n\
GPU 0, 29418, python train.py, 11840\r\n\
GPU 1, 18302, python eval.py, 2048\r\n\
__RSSH_RESOURCE_CPU__\r\n\
    PID USER     COMMAND         %CPU %MEM\r\n\
  29418 alice    python          66.2 18.4\r\n\
  18302 researcher python        21.5  7.1\r\n\
    911 root     systemd          4.0  1.0\r\n\
__RSSH_RESOURCE_JOBS__\r\n\
102934|gpu|train.sh|alice|RUNNING|2:13:05|4|gpu[01-04]\r\n\
102935|gpu|eval.py|researcher|PENDING|0:00|1|Priority\r\n\
__RSSH_RESOURCE_DISK__\r\n\
/dev/nvme0n1|1.8T|0.9T|0.8T|53%|/home\r\n\
scratch|50T|31T|19T|62%|/scratch\r\n\
__RSSH_RESOURCE_END__\r\n"
            .to_string(),
        c if c.contains("python") && c.contains(".py") => format!(
            "模拟运行：{c}\r\n\
             epoch 1/3 - loss=0.420 - device={}\r\n\
             epoch 2/3 - loss=0.310\r\n\
             epoch 3/3 - loss=0.260\r\n\
             done\r\n",
            if c.contains("CUDA_VISIBLE_DEVICES=''") {
                "CPU"
            } else {
                "GPU"
            }
        ),
        c => format!("未找到命令：{c}\r\n"),
    };
    line
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::provider::RsProviderKind;
    use tokio::sync::mpsc;

    fn cfg(host: &str) -> ConnectionConfig {
        ConnectionConfig {
            host: host.into(),
            port: 22,
            username: "researcher".into(),
            kind: RsProviderKind::Mock,
        }
    }

    #[tokio::test]
    async fn connect_emits_banner_and_send_echoes() {
        let (tx, mut rx) = mpsc::unbounded_channel();
        let sink = ProviderSink::new(tx);
        let mut p = MockProvider::new(cfg("hpc.example.edu"));

        p.connect(sink).await.expect("connect ok");
        let banner = rx.recv().await.expect("banner");
        match banner {
            ProviderEvent::Data(d) => {
                assert!(String::from_utf8_lossy(&d).contains("hpc.example.edu"))
            }
            _ => panic!("expected data banner"),
        }

        p.send(b"nvidia-smi\n").await.expect("send ok");
        let echo = rx.recv().await.expect("echo");
        match echo {
            ProviderEvent::Data(d) => {
                let s = String::from_utf8_lossy(&d);
                assert!(s.contains("nvidia-smi"));
                assert!(s.contains("NVIDIA"));
            }
            _ => panic!("expected data echo"),
        }
    }

    #[tokio::test]
    async fn connect_fails_for_fail_hosts() {
        let (tx, _rx) = mpsc::unbounded_channel();
        let sink = ProviderSink::new(tx);
        let mut p = MockProvider::new(cfg("fail.cluster"));
        let err = p.connect(sink).await.unwrap_err();
        assert_eq!(err.code, RsErrorCode::ConnectFailed);
    }
}
