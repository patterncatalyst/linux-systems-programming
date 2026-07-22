// proto.go — the canonical chatterd chat frame (introduced ch21, unchanged
// since): magic "CH", version 1, one byte type, big-endian u16 length, then
// payload. This capstone speaks only JOIN/MSG/DELIVER, the same three types
// every prior chatterd chapter uses.
package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

const (
	frameMagic0 = 'C'
	frameMagic1 = 'H'
	frameVer    = 1

	typeJoin    = 1
	typeMsg     = 2
	typeDeliver = 3
)

type frame struct {
	typ     byte
	payload []byte
}

func writeFrame(w io.Writer, f frame) error {
	if len(f.payload) > 0xFFFF {
		return fmt.Errorf("payload too large: %d bytes", len(f.payload))
	}
	hdr := make([]byte, 6)
	hdr[0], hdr[1] = frameMagic0, frameMagic1
	hdr[2] = frameVer
	hdr[3] = f.typ
	binary.BigEndian.PutUint16(hdr[4:], uint16(len(f.payload)))
	if _, err := w.Write(hdr); err != nil {
		return fmt.Errorf("write header: %w", err)
	}
	if len(f.payload) > 0 {
		if _, err := w.Write(f.payload); err != nil {
			return fmt.Errorf("write payload: %w", err)
		}
	}
	return nil
}

var errBadMagic = errors.New("bad frame magic/version")

func readFrame(r io.Reader) (frame, error) {
	hdr := make([]byte, 6)
	if _, err := io.ReadFull(r, hdr); err != nil {
		return frame{}, err
	}
	if hdr[0] != frameMagic0 || hdr[1] != frameMagic1 || hdr[2] != frameVer {
		return frame{}, errBadMagic
	}
	n := binary.BigEndian.Uint16(hdr[4:])
	payload := make([]byte, n)
	if n > 0 {
		if _, err := io.ReadFull(r, payload); err != nil {
			return frame{}, fmt.Errorf("read payload: %w", err)
		}
	}
	return frame{typ: hdr[3], payload: payload}, nil
}

// deliverPayload builds the ch21 "nick NUL text" DELIVER payload.
func deliverPayload(nick, text string) []byte {
	b := make([]byte, 0, len(nick)+1+len(text))
	b = append(b, nick...)
	b = append(b, 0)
	b = append(b, text...)
	return b
}

// splitDeliver reverses deliverPayload; ok=false if there is no NUL.
func splitDeliver(payload []byte) (nick, text string, ok bool) {
	for i, c := range payload {
		if c == 0 {
			return string(payload[:i]), string(payload[i+1:]), true
		}
	}
	return "", "", false
}
