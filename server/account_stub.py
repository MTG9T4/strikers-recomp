"""Job 18 replacement boundary: issue and validate our opaque account ID token.

Development provider only: a local operator chooses a principal. This is not
Internet registration, password authentication, friends or an entitlement check.
No dependency, network request, or Nintendo credential is involved.
"""
import argparse
import base64
import hashlib
import hmac
import json
from pathlib import Path
import secrets
import time

AUDIENCE = 'strikers-local-nex'
ISSUER = 'strikers-local-account-v1'


def b64(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode('ascii')


def unb64(data):
    if not isinstance(data, str) or len(data) > 4096:
        raise ValueError('invalid base64 field')
    return base64.b64decode(data + '=' * (-len(data) % 4), altchars=b'-_', validate=True)


class LocalAccountProvider:
    def __init__(self, key):
        if len(key) != 32:
            raise ValueError('account key must be 32 random bytes')
        self.key = key

    def issue(self, principal, lifetime=900, now=None):
        if not isinstance(principal, int) or isinstance(principal, bool) or not 0 < principal < 2**64:
            raise ValueError('principal must be a nonzero u64')
        if not 1 <= lifetime <= 3600:
            raise ValueError('token lifetime must be 1..3600 seconds')
        now = int(time.time()) if now is None else now
        header = b64(b'{"alg":"HS256","typ":"JWT"}')
        claims = {'iss': ISSUER, 'aud': AUDIENCE, 'sub': str(principal),
                  'iat': now, 'exp': now + lifetime, 'jti': secrets.token_hex(16)}
        payload = b64(json.dumps(claims, separators=(',', ':'), sort_keys=True).encode())
        message = header + '.' + payload
        return message + '.' + b64(hmac.digest(self.key, message.encode(), 'sha256'))

    def verify(self, token, now=None):
        if not isinstance(token, str) or len(token) > 3072:
            raise ValueError('invalid token')
        try:
            header, payload, signature = token.split('.')
            expected = hmac.digest(self.key, (header + '.' + payload).encode('ascii'), 'sha256')
            if not hmac.compare_digest(expected, unb64(signature)):
                raise ValueError('invalid signature')
            if json.loads(unb64(header)) != {'alg': 'HS256', 'typ': 'JWT'}:
                raise ValueError('invalid algorithm')
            claims = json.loads(unb64(payload))
            now = int(time.time()) if now is None else now
            if claims['iss'] != ISSUER or claims['aud'] != AUDIENCE:
                raise ValueError('invalid issuer or audience')
            if type(claims['iat']) is not int or type(claims['exp']) is not int:
                raise ValueError('invalid timestamps')
            if not claims['iat'] <= now < claims['exp'] <= claims['iat'] + 3600:
                raise ValueError('expired or future token')
            principal = int(claims['sub'])
            if str(principal) != claims['sub'] or not 0 < principal < 2**64:
                raise ValueError('invalid principal')
            if len(bytes.fromhex(claims['jti'])) != 16:
                raise ValueError('invalid token id')
            return principal
        except (KeyError, TypeError, UnicodeError, OverflowError) as e:
            raise ValueError('invalid token fields') from e

    def write_runtime_token(self, path, principal, lifetime=900):
        token = self.issue(principal, lifetime)
        claims = json.loads(unb64(token.split('.')[1]))
        # Exclusive creation protects existing profiles/credentials from overwrite.
        with Path(path).open('x', encoding='ascii', newline='\n') as f:
            f.write(f'STRIKERS-LOCAL-TOKEN-V1\n{principal}\n{claims["exp"]}\n{token}\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--key', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--principal', type=int, required=True)
    ap.add_argument('--lifetime', type=int, default=900)
    args = ap.parse_args()
    if not args.key.exists():
        args.key.parent.mkdir(parents=True, exist_ok=True)
        with args.key.open('xb') as f:
            f.write(secrets.token_bytes(32))
        args.key.chmod(0o600)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    LocalAccountProvider(args.key.read_bytes()).write_runtime_token(args.out, args.principal, args.lifetime)
    args.out.chmod(0o600)
    print(f'Issued local principal {args.principal}; token saved to {args.out} (credential not printed)')


if __name__ == '__main__':
    main()
