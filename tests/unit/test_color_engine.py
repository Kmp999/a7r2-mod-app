#!/usr/bin/env python3
import unittest

def clamp_u8(val):
    return max(0, min(255, val))

def matrix_apply(m, r, g, b):
    rout = (m[0][0] * r + m[0][1] * g + m[0][2] * b + 128) >> 8
    gout = (m[1][0] * r + m[1][1] * g + m[1][2] * b + 128) >> 8
    bout = (m[2][0] * r + m[2][1] * g + m[2][2] * b + 128) >> 8
    return clamp_u8(rout), clamp_u8(gout), clamp_u8(bout)

class TestColorEngine(unittest.TestCase):
    def test_identity_matrix(self):
        identity = [[256, 0, 0], [0, 256, 0], [0, 0, 256]]
        r, g, b = matrix_apply(identity, 128, 64, 32)
        self.assertEqual((r, g, b), (128, 64, 32))

    def test_clamping_upper(self):
        boost = [[512, 0, 0], [0, 512, 0], [0, 0, 512]]
        r, g, b = matrix_apply(boost, 200, 200, 200)
        self.assertEqual((r, g, b), (255, 255, 255))

    def test_clamping_lower(self):
        neg = [[-256, 0, 0], [0, -256, 0], [0, 0, -256]]
        r, g, b = matrix_apply(neg, 50, 50, 50)
        self.assertEqual((r, g, b), (0, 0, 0))

    def test_provia_bounds(self):
        provia = [[266, -5, -5], [-5, 266, -5], [-5, -5, 266]]
        for val in [0, 64, 128, 192, 255]:
            r, g, b = matrix_apply(provia, val, val, val)
            self.assertTrue(0 <= r <= 255)
            self.assertTrue(0 <= g <= 255)
            self.assertTrue(0 <= b <= 255)

if __name__ == '__main__':
    unittest.main()
