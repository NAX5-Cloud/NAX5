"""Print what the backend recorded for the integration run (JSON).

    python manage.py shell -c "exec(open(r'<this file>').read())"
Environment: NAX5_IT_USER_A, NAX5_IT_USER_B, NAX5_IT_SESSIONS (comma-separated public ids).
"""
import json
import os

from apps.client_reports.models import ClientReport
from apps.client_telemetry.models import ClientEvent
from apps.sessions.models import Session

ids = [s for s in os.environ.get("NAX5_IT_SESSIONS", "").split(",") if s]
sessions = {
    str(s.public_id): {
        "status": s.status,
        "end_reason_code": s.end_reason_code,
        "played_seconds": s.played_seconds,
        "has_started": s.started_at is not None,
    }
    for s in Session.objects.filter(public_id__in=ids)
}
report = (
    ClientReport.objects.filter(user__email=os.environ["NAX5_IT_USER_A"], kind="crash")
    .order_by("-received_at")
    .first()
)
print("CHECK_JSON=" + json.dumps({
    "play_requested_events": ClientEvent.objects.filter(user__email=os.environ["NAX5_IT_USER_A"], event_type="PLAY_REQUESTED").count(),
    "sessions": sessions,
    "crash_report": None if report is None else {
        "is_archive": report.is_archive,
        "stored_size": report.stored_size,
        "preview_has_dump_placeholder": "(crash dump," in (report.preview_tail or ""),
        "preview_has_summary": "exception_code=0xC0000005" in (report.preview_tail or ""),
    },
}))
