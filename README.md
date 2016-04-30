#osnet project 1

Questions?

Q: What happens if seqno overflows?

A: See project requirements (page 2, bullet 6): [...] You do not have to handle sequence number
overflowing and wrapping in the lifetime of a connection.

Q: What is the byte-order of the Payload? (Documentation says nothing)

A: The payload is an array of chars which are not affected by the byte-order in the slightest ;)
