# BruceLink Agent Skills

Portable, project-specific skills in the standard **SKILL.md** format (YAML
frontmatter with `name` + `description`, then a markdown body). They are usable
by **Hermes Agent** and any coding assistant that supports the SKILL.md
specification. Each skill is focused and independently loadable, and references
`docs/handoff/` rather than duplicating it.

These skills are documentation-derived: they encode BruceLink's verified
workflows, the ADR/safety boundaries, and the known pitfalls from development.
They preserve the project's authorization and safety boundaries — none weaken a
boundary. Keep them in sync with the handoff via `brucelink-handoff-maintenance`
when a workflow changes.

## The skills

| Skill (dir = `name`) | Use it to… |
|---|---|
| `brucelink-onboarding` | Orient a new dev/agent and confirm the known-good baseline. |
| `brucelink-build-validation` | Reproducibly build + test and compare to the baseline. |
| `brucelink-memory-regression` | Detect/analyze StickS3 internal-RAM regressions. |
| `brucelink-regression-testing` | Add/run host regression tests (reproduce-against-old-code). |
| `brucelink-hardware-test-report` | Record a hardware result, preserving the verified/owner-reported line. |
| `brucelink-adr-compliance` | Check a change against the ADRs and safety boundaries. |
| `brucelink-session-checkpoint` | Checkpoint or recover session state between agents/humans. |
| `brucelink-handoff-maintenance` | Keep `docs/handoff/` + `AGENTS.md`/`CLAUDE.md` consistent. |
| `brucelink-upstream-review` | Review Bruce/dependency compatibility before adopting a bump. |

## Layout

```
docs/agent-skills/
├── README.md                       (this file)
└── <skill-name>/
    └── SKILL.md                    (frontmatter: name, description; body: the procedure)
```

Each `SKILL.md` body has: When to invoke · Required project context · References ·
Procedure · Verification / acceptance · Common mistakes & pitfalls · Expected
output. Only `name` and `description` are used in frontmatter, so the files stay
portable across agents (no agent-specific metadata).

## Install & invoke — Hermes Agent

Hermes discovers skills in `~/.hermes/skills/` (its "source of truth"), in a
project-local `<repo>/.hermes/skills/` (after you trust the repo), or in any
directory added via config. Pick one:

**Option A — point Hermes at this directory (no copy).** In your Hermes config:
```yaml
skills:
  external_dirs:
    - /absolute/path/to/brucelink/docs/agent-skills
```

**Option B — project-local (keeps them with the repo, trusted per-project):**
```bash
cd /path/to/brucelink
mkdir -p .hermes/skills
ln -s "$(pwd)/docs/agent-skills/"brucelink-* .hermes/skills/
hermes skills trust            # trust this repo's project-local skills
```

**Option C — user-global (symlink into the source of truth):**
```bash
ln -s "/path/to/brucelink/docs/agent-skills/"brucelink-* ~/.hermes/skills/
```

Verify and invoke:
```bash
hermes skills list                                  # the brucelink-* skills should appear
hermes chat --toolsets skills -q "What skills do you have?"
```
In a message, invoke a skill as a slash command, e.g.:
```
/brucelink-onboarding
/brucelink-build-validation validate the current tree against the baseline
```

Reference: Hermes Agent — Skills System,
<https://hermes-agent.nousresearch.com/docs/user-guide/features/skills>.

## Install & invoke — other SKILL.md-compatible assistants

These are plain SKILL.md directories. For any agent that supports the spec,
either point it at `docs/agent-skills/` as a skills source (per that agent's
configuration) or copy/symlink the individual `brucelink-*` directories into the
agent's skills directory. Discovery/triggering is driven by the frontmatter
`name`/`description`; the body is the procedure the agent follows. No
Hermes-specific fields are required.
