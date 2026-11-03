# SFCOP-decomp — Design

What the system does and how it behaves. This document defines behavior; `CONVENTIONS.md` defines form; `PLAN.md` defines the order of work. Sections are numbered and stay numbered, because briefs and reviews cite them by number (`DESIGN §4.2`).

How to fill this in: one section at a time, in a design conversation with the owner. Write each behavior concretely enough that a test can be named for it. Where a decision is not made, write it in §12 rather than guessing.

---

## 1. Purpose

<!-- One paragraph: what it is, who it is for, the one hard constraint it must meet. -->

## 2. What it does and does not do

<!-- Two short lists. The "does not" list is what keeps the scope from growing. -->

## 3. Constraints

<!-- Cost, platform, performance, privacy, compatibility, licensing. Each with a number where one exists. -->

## 4. Domain model

<!-- The entities, their fields, their states and the transitions between states. Invariants worth a test each. -->

## 5. Behaviors

<!-- One subsection per feature or flow. Inputs, outputs, edge cases, failure handling. Number them; they will be cited. -->

## 6. Interfaces

<!-- APIs, command lines, file formats, user interface pages. Contracts that two parts of the system share, which the foundation wave will freeze. -->

## 7. Architecture

<!-- Components and how they talk. What is shared and what is not. Layering rules. -->

## 8. Data and storage

<!-- Where state lives, how it is identified, how it changes, how it is backed up. -->

## 9. Security and privacy

<!-- Who can do what; where secrets live; what is never logged or committed. -->

## 10. Operations

<!-- Deployment, monitoring, alerts, cost tracking, runbook topics. -->

## 11. Cost

<!-- Expected monthly figure and what it is made of. "Free" is a number too. -->

## 12. Open questions

<!-- Each with who decides and what it blocks. An open question is written as open; a document never says the owner accepted something they have not. -->
