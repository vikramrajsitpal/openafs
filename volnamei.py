#!/usr/bin/python3

import string

forward = ['+', '='] + list(string.digits) + list(string.ascii_uppercase) + list(string.ascii_lowercase)

def num_to_char(num):
    chars = []
    if num == 0:
        chars.append(forward[0])
    else:
        while num != 0:
            chars.append(forward[num & 0x3f])
            num >>= 6
    return ''.join(chars)

def volumeid_to_path(partid, volid):
    return '/vicep{0}/AFSIDat/{1}/{2}'.format(partid, num_to_char(volid & 0xff), num_to_char(volid))

path = volumeid_to_path('a', 536870927)
print(path)