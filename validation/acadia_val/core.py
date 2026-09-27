"""core.py — result model, reporter, runner and shared context."""

from __future__ import annotations

import json
import sys
import time
import traceback
from dataclasses import dataclass, field
from enum import Enum
from typing import Callable, List, Optional

# ---------------------------------------------------------------------------
# Result model
# ---------------------------------------------------------------------------

class Status(str, Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    WARN = "WARN"
    SKIP = "SKIP"


@dataclass
class TestResult:
    suite: str
    idx: int
    name: str
    status: Status
    detail: str = ""
    duration: float = 0.0

    @property
    def tid(self) -> str:
        return f"{self.suite.upper()}-{self.idx:02d}"

    def as_dict(self) -> dict:
        return {
            "id": self.tid,
            "suite": self.suite,
            "name": self.name,
            "status": self.status.value,
            "detail": self.detail,
            "duration_s": round(self.duration, 3),
        }


# ANSI colour helpers -------------------------------------------------------

class C:
    RESET = "\033[0m"
    RED = "\033[31m"
    GREEN = "\033[32m"
    YELLOW = "\033[33m"
    CYAN = "\033[36m"
    GREY = "\033[90m"
    BOLD = "\033[1m"

    @classmethod
    def enabled(cls) -> bool:
        return sys.stdout.isatty()


def _paint(status: Status, text: str) -> str:
    if not C.enabled():
        return text
    colour = {
        Status.PASS: C.GREEN,
        Status.FAIL: C.RED,
        Status.WARN: C.YELLOW,
        Status.SKIP: C.CYAN,
    }[status]
    return f"{colour}{text}{C.RESET}"


_SYMBOL = {
    Status.PASS: "PASS",
    Status.FAIL: "FAIL",
    Status.WARN: "WARN",
    Status.SKIP: "SKIP",
}


# ---------------------------------------------------------------------------
# Reporter — one per suite
# ---------------------------------------------------------------------------

class Reporter:
    """Collects results for one suite and prints them live."""

    def __init__(self, suite: str, verbose: bool = False):
        self.suite = suite
        self.verbose = verbose
        self.results: List[TestResult] = []
        self._n = 0

    # -- low level ----------------------------------------------------------
    def record(self, name: str, status: Status, detail: str = "",
               duration: float = 0.0) -> TestResult:
        self._n += 1
        r = TestResult(self.suite, self._n, name, status, detail, duration)
        self.results.append(r)
        self._print(r)
        return r

    def _print(self, r: TestResult) -> None:
        line = f"  [{_paint(r.status, _SYMBOL[r.status])}] {r.tid:<8} {r.name}"
        if r.detail and (self.verbose or r.status is not Status.PASS):
            first = r.detail.strip().splitlines()[0] if r.detail.strip() else ""
            if len(first) > 220:
                first = first[:217] + "..."
            line += f"  {C.GREY}— {first}{C.RESET}" if C.enabled() else f"  — {first}"
        print(line, flush=True)

    # -- high level ---------------------------------------------------------
    def check(self, name: str, ok_or_fn=None, detail: str = "", *,
              warn_only: bool = False, skip: Optional[str] = None) -> Optional[TestResult]:
        """Two call styles:

            rep.check("name", callable_fn)          # fn() -> (ok, detail) | bool
            rep.check("name", ok_bool, "detail")    # direct result

        Exceptions inside a callable are converted to FAIL (or WARN when
        warn_only).
        """
        if skip:
            return self.record(name, Status.SKIP, skip)
        t0 = time.monotonic()
        if callable(ok_or_fn):
            try:
                res = ok_or_fn()
                if isinstance(res, tuple) and len(res) == 2:
                    ok, d = bool(res[0]), str(res[1] or "")
                else:
                    ok, d = bool(res), ""
            except Exception as e:  # noqa: BLE001 — the suite must survive one bad test
                ok, d = False, f"exception: {e}"
                if self.verbose:
                    d += "\n" + "".join(traceback.format_exc(limit=3))
        else:
            ok = bool(ok_or_fn)
            d = detail
        dur = time.monotonic() - t0
        if ok:
            status = Status.PASS
        elif warn_only:
            status = Status.WARN
        else:
            status = Status.FAIL
        return self.record(name, status, d, dur)

    def skip(self, name: str, reason: str) -> TestResult:
        return self.record(name, Status.SKIP, reason)

    def summary(self):
        p = sum(1 for r in self.results if r.status is Status.PASS)
        f = sum(1 for r in self.results if r.status is Status.FAIL)
        w = sum(1 for r in self.results if r.status is Status.WARN)
        s = sum(1 for r in self.results if r.status is Status.SKIP)
        return p, f, w, s


# ---------------------------------------------------------------------------
# Suite base + shared context
# ---------------------------------------------------------------------------

class Suite:
    NAME = "suite"

    def __init__(self, ctx: "Context"):
        self.ctx = ctx
        self.rep = Reporter(self.NAME, verbose=ctx.opts.verbose)

    def run(self) -> None:
        raise NotImplementedError


class Context:
    """Shared state handed to every suite."""

    def __init__(self, opts, target):
        self.opts = opts
        self.target = target          # acadia_val.target.Target (may be None in selftest)
        self.facts: dict = {}          # facts discovered by earlier suites

    # convenience accessors for facts
    def fact(self, key, default=None):
        return self.facts.get(key, default)


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

class Runner:
    def __init__(self, ctx: Context, suite_classes: list, stop_on_critical: bool = True):
        self.ctx = ctx
        self.suite_classes = suite_classes
        self.stop_on_critical = stop_on_critical

    def run(self) -> List[TestResult]:
        all_results: List[TestResult] = []
        for cls in self.suite_classes:
            print(f"\n{_paint(Status.PASS, '')}[suite] {cls.NAME}", flush=True)
            suite = cls(self.ctx)
            try:
                suite.run()
            except Exception as e:  # noqa: BLE001
                suite.rep.record(
                    f"{cls.NAME} aborted", Status.FAIL,
                    f"suite crashed: {e}"
                    + ("".join(traceback.format_exc(limit=4)) if self.ctx.opts.verbose else ""),
                )
            all_results.extend(suite.rep.results)

            # A hard SSH failure or too many failures aborts the remaining suites.
            fails = sum(1 for r in suite.rep.results if r.status is Status.FAIL)
            if self.stop_on_critical and fails > 0 and cls.NAME in ("env",):
                print("  [suite] env suite reported failures — continuing anyway "
                      "(failures do not block other suites)", flush=True)
        return all_results


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

def print_summary(results: List[TestResult]) -> bool:
    p = sum(1 for r in results if r.status is Status.PASS)
    f = sum(1 for r in results if r.status is Status.FAIL)
    w = sum(1 for r in results if r.status is Status.WARN)
    s = sum(1 for r in results if r.status is Status.SKIP)
    total = len(results)
    print()
    bar = "=" * 64
    print(bar)
    print(f"  VALIDATION SUMMARY — {total} checks   "
          f"{_paint(Status.PASS, f'{p} passed')}  "
          f"{_paint(Status.FAIL, f'{f} failed')}  "
          f"{_paint(Status.WARN, f'{w} warned')}  "
          f"{_paint(Status.SKIP, f'{s} skipped')}")
    print(bar)
    if f:
        print("  Failed checks:")
        for r in results:
            if r.status is Status.FAIL:
                print(f"    {r.tid:<8} {r.name}")
                if r.detail:
                    for ln in r.detail.strip().splitlines()[:4]:
                        print(f"             {ln}")
    ok = f == 0
    verdict = _paint(Status.PASS, "OVERALL: PASS") if ok else _paint(Status.FAIL, "OVERALL: FAIL")
    print(f"  {verdict}")
    print(bar, flush=True)
    return ok


def write_json(results: List[TestResult], path: str) -> None:
    data = {
        "suite_version": "1.0.0",
        "generated_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "results": [r.as_dict() for r in results],
    }
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)


def _xml_escape(s: str) -> str:
    return (s.replace("&", "&amp;").replace("<", "&lt;")
             .replace(">", "&gt;").replace('"', "&quot;"))


def write_junit(results: List[TestResult], path: str) -> None:
    lines = ['<?xml version="1.0" encoding="UTF-8"?>']
    p = sum(1 for r in results if r.status is Status.PASS)
    f = sum(1 for r in results if r.status is Status.FAIL)
    s = sum(1 for r in results if r.status is Status.SKIP)
    suites = sorted({r.suite for r in results})
    lines.append(f'<testsuites tests="{len(results)}" failures="{f}" skipped="{s}" passed="{p}">')
    for su in suites:
        sub = [r for r in results if r.suite == su]
        pf = sum(1 for r in sub if r.status is Status.FAIL)
        ps = sum(1 for r in sub if r.status is Status.SKIP)
        dur = sum(r.duration for r in sub)
        lines.append(f'  <testsuite name="{_xml_escape(su)}" '
                     f'tests="{len(sub)}" failures="{pf}" skipped="{ps}" '
                     f'time="{dur:.3f}">')
        for r in sub:
            lines.append(f'    <testcase name="{_xml_escape(r.tid + " " + r.name)}" '
                         f'classname="{_xml_escape(su)}" time="{r.duration:.3f}">')
            if r.status is Status.FAIL:
                lines.append(f'      <failure message="{_xml_escape(r.detail[:400])}">'
                             f'{_xml_escape(r.detail)}</failure>')
            elif r.status is Status.SKIP:
                lines.append(f'      <skipped message="{_xml_escape(r.detail[:200])}"/>')
            elif r.status is Status.WARN:
                lines.append(f'      <system-out>{_xml_escape(r.detail[:800])}</system-out>')
            lines.append('    </testcase>')
        lines.append('  </testsuite>')
    lines.append('</testsuites>')
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines) + "\n")