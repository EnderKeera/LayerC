<!-- desktop-evidence/1 -->
Example evidence for unit tests.
```json
{
  "schema": "desktop-evidence/1",
  "project": "Example/Project",
  "commit": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "recordedAt": "2026-05-20T12:34:56Z",
  "harness": {
    "id": "demo-desktop",
    "version": "1.0"
  },
  "build": {
    "source": "ci-artifact",
    "ciRunId": 123456,
    "builtCommit": "cccccccccccccccccccccccccccccccccccccccc",
    "identitySha256": "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
  },
  "environment": {
    "os": "Example OS 1",
    "session": "demo-x11",
    "machine": "physical",
    "gpu": "Example GPU"
  },
  "results": [
    {
      "id": "capture",
      "status": "pass",
      "detail": "Visible window captured"
    },
    {
      "id": "overlay",
      "status": "pass"
    },
    {
      "id": "latency",
      "status": "skip",
      "detail": "Optional measurement not run"
    }
  ],
  "unexpected": true
}
```
