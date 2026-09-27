"""Seed a LOCAL nax5-backend database for the client integration test.

Run from the nax5-backend checkout:
    python manage.py shell -c "exec(open(r'<this file>').read())"
Environment: NAX5_IT_USER_A, NAX5_IT_USER_B, NAX5_IT_PASSWORD, NAX5_IT_CONSOLE.
Refuses to run unless DATABASE_URL points at 127.0.0.1/localhost.
"""
import json
import os

from allauth.account.models import EmailAddress
from django.conf import settings
from django.contrib.auth import get_user_model

from apps.accounts.models import Profile
from apps.consoles.models import Console
from apps.consoles.services.material import RP_MORNING_SIZE, RP_REGIST_KEY_SIZE
from apps.consoles.services.secret_provider import ConsoleConnectionMaterialProvider
from apps.sessions.models import OCCUPYING_SESSION_STATUSES, Session

db_host = settings.DATABASES["default"].get("HOST", "")
if db_host not in ("127.0.0.1", "localhost"):
    raise SystemExit(f"refusing to seed non-local database host {db_host!r}")

password = os.environ["NAX5_IT_PASSWORD"]
console_code = os.environ["NAX5_IT_CONSOLE"]
users = []
for email in (os.environ["NAX5_IT_USER_A"], os.environ["NAX5_IT_USER_B"]):
    user, _ = get_user_model().objects.get_or_create(email=email)
    user.set_password(password)
    user.is_active = True
    user.save()
    profile, _ = Profile.objects.get_or_create(user=user)
    profile.access_status = Profile.AccessStatus.ACTIVE
    profile.save()
    EmailAddress.objects.update_or_create(user=user, email=email, defaults={"verified": True, "primary": True})
    users.append(user)

# Only the test console may be READY, so allocation is deterministic.
Console.objects.exclude(code=console_code).update(operational_state=Console.OperationalState.DISABLED)
console, _ = Console.objects.get_or_create(code=console_code, defaults={"name": "Integration", "region": "Local"})
console.public_host = "203.0.113.10"
console.operational_state = Console.OperationalState.READY
console.save()
ConsoleConnectionMaterialProvider().put(console.pk, {
    "version": 1,
    "target": 1000100,
    "nickname": "Integration",
    "regist_key": b"A" * RP_REGIST_KEY_SIZE,
    "morning": b"B" * RP_MORNING_SIZE,
    "console_pin": "",
})
# Close leftovers from earlier runs without deleting history other rows point to.
Session.objects.filter(status__in=OCCUPYING_SESSION_STATUSES).filter(user__in=users).update(status=Session.Status.ENDED)
Session.objects.filter(status__in=OCCUPYING_SESSION_STATUSES, console=console).update(status=Session.Status.ENDED)
print(json.dumps({"seeded_users": [u.email for u in users], "console": console.code}))
