# Lode Coding

> A practical method for using AI coding assistants without giving up cognitive ownership, professional accountability, or hard-earned engineering practice.

---

## What It Is

Lode Coding is a structured documentation approach for AI-assisted software development. It keeps a focused set of markdown files alongside the codebase that describe the system, the patterns that matter, and the decisions the assistant will need to make good choices in future sessions.

The name comes from mining: a **lode** is a rich vein of valuable material. In this case, the valuable material is project knowledge — architecture, terminology, constraints, patterns, tradeoffs, lessons learned, and the shape of the system as it exists today.

## Core Philosophy

- **Not a documentation job for the engineer.** The lode is created and updated as a byproduct of working with the agent: planning, clarifying, correcting, reviewing, and implementing.
- **The engineer provides judgment, direction, and standards. The agent turns the resulting knowledge into durable project memory.**
- **Cognitive ownership is the goal.** The engineer retains understanding and accountability for all code, even when AI assists.
- **Continuity across sessions.** The lode keeps knowledge alive, keeps judgment in play, and helps preserve the accountability that makes professional software engineering possible.

## Minimal Structure

```
project/
└── lode/
    ├── summary.md          # Project overview, scope, key decisions
    ├── terminology.md      # Shared glossary
    ├── practices.md        # Coding style, constraints, patterns
    ├── lode-map.md         # Index of all lode files
    └── [subsystem folders] # Focused documentation per module
```

## Where to Learn More

The authoritative source on Lode Coding is maintained by @fjzeit:

**https://fjzeit.github.io/lode**

---

*This file exists so that every new session has a single reference point for how this project uses the Lode Coding methodology.*
