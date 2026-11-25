<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# libddbsh

`libddbsh` is another name for [`ddbsh`](https://pypi.org/project/ddbsh/),
the DynamoDB shell as a Python library. Installing it installs `ddbsh` at the
same version, and `import libddbsh` gives the same objects as `import ddbsh`.

```
pip install ddbsh
```

```python
import ddbsh

conn = ddbsh.connect()
print(conn.run('select * from movies where id = "m1";'))
```

New code should install and import `ddbsh`. The guide is
[docs/PYTHON.md](https://github.com/awslabs/dynamodb-shell/blob/main/docs/PYTHON.md).
