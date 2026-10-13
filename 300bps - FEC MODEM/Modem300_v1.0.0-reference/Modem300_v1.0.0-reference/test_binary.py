import unittest
import binary_test as bench


class FramingTests(unittest.TestCase):
    def test_all_byte_values_and_round_trip(self):
        for role in (1, 2):
            stream, bodies = bench.make_stream(role, 32)
            self.assertEqual(set(b"".join(x[3:] for x in bodies[:16])), set(range(256)))
            decoded, invalid = bench.parse(stream)
            self.assertEqual(decoded, bodies)
            self.assertEqual(invalid, 0)
            summary = bench.summarize(stream, bodies, stream)
            self.assertTrue(summary["exact_stream"])
            self.assertEqual(summary["exact_frames"], 32)

    def test_deletion_is_not_labeled_a_bit_error(self):
        stream, bodies = bench.make_stream(1, 32)
        damaged = stream[:12] + stream[13:]
        result = bench.summarize(stream, bodies, damaged)
        self.assertTrue(result["received_is_subsequence_of_sent"])
        self.assertEqual(result["received_bytes"], result["sent_bytes"]-1)
        self.assertEqual(result["crc_valid_wrong_payload"], 0)
        self.assertGreater(result["missing_frames"], 0)

    def test_corruption_and_duplicates(self):
        stream, bodies = bench.make_stream(2, 32)
        damaged = bytearray(stream)
        damaged[12] ^= 1
        self.assertGreater(bench.summarize(stream, bodies, damaged)["invalid_frames"], 0)
        doubled = bench.summarize(stream, bodies, stream+stream)
        self.assertEqual(doubled["duplicates"], 32)

    def test_valid_crc_with_wrong_payload(self):
        stream, bodies = bench.make_stream(1, 32)
        wrong = bodies[0][:-1] + bytes([bodies[0][-1] ^ 1])
        result = bench.summarize(stream, bodies, bench.frame(wrong))
        self.assertEqual(result["crc_valid_wrong_payload"], 1)

    def test_empty_stream(self):
        stream, bodies = bench.make_stream(1, 32)
        self.assertEqual(bench.summarize(stream, bodies, b"")["missing_frames"], 32)


if __name__ == "__main__":
    unittest.main()
