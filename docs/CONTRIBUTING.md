<!--
Copyright 2026 ddbsh reimplementation contributors.
SPDX-License-Identifier: Apache-2.0
-->

# Contributing

If you find bugs or errors of any kind, please do let the maintainers know. If you are so inclined, please let the maintainers know that you would like to submit pull-requests. We will do our best to respond in a timely manner and where possible accept pull-requests and changes.

## Direction

Here are some things that need to be done:

1. Complete support for AM-CMK and CM-CMK. There is code in CREATE TABLE and ALTER TABLE to handle this but it has not been exhaustively tested (and that is the reason it does not show up in the HELP or this README). More work is required to ensure that it is performing as expected.

2. Update online help and README.

3. Add support for additional language constructs. This is a delicate balancing act between attempting to build all of SQL against DynamoDB (which we don't want to do) and providing some more SQL constructs which will help users. For example, we do intend to add support for client side implementations of COUNT and COUNT DISTINCT. Once COUNT DISTINCT is implemented, it is a small matter of providing language support for GROUP BY which is something that I do want to do. However, I explicitly do not want to add support for client-side joins - they are not a construct that is natively supported by DynamoDB. This is, however, a matter of some heated discussions among the development team.

4. Add support for DynamoDB DAX.

5. Add more tests.

6. Distribute signed binaries.

