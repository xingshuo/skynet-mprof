package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"io/ioutil"
	"os"
	"sort"
	"strings"
	"github.com/goccy/go-graphviz"
)

const (
	DataHeaderLen = 4
	KRED          = "\x1B[31m"
	KNRM          = "\x1B[0m"
)

var (
	input string
	inuse bool
	text  bool
	png   string
	svg   string
	info  string
)

type Sample struct {
	alloc_objs  int
	alloc_bytes int
	free_objs   int
	free_bytes  int

	depth int
	stack []uint64
}

type Profile struct {
	funcName2Id map[string]uint64
	funcId2Name map[uint64]string
	samples     []*Sample
}

func parseFile(filename string) (*Profile, error) {
	data, err := ioutil.ReadFile(filename)
	if err != nil {
		return nil, fmt.Errorf("ReadFile fail: %w", err)
	}
	totalLen := len(data)
	if totalLen < DataHeaderLen {
		return nil, fmt.Errorf("header len error")
	}
	bodyLen := binary.BigEndian.Uint32(data)
	if totalLen != DataHeaderLen+int(bodyLen) {
		return nil, fmt.Errorf("data len error")
	}
	prof := &Profile{
		funcName2Id: make(map[string]uint64),
		funcId2Name: make(map[uint64]string),
		samples:     make([]*Sample, 0),
	}
	offset := DataHeaderLen
	funcNum := binary.BigEndian.Uint32(data[offset:])
	offset += 4
	for i := 0; i < int(funcNum); i++ {
		nameLen := int(data[offset])
		offset += 1
		name := string(data[offset : offset+nameLen])
		offset += nameLen
		Id := binary.BigEndian.Uint64(data[offset:])
		offset += 8
		prof.funcName2Id[name] = Id
		prof.funcId2Name[Id] = name
	}
	for offset < totalLen {
		sa := &Sample{
			alloc_objs:  0,
			alloc_bytes: 0,
			free_objs:   0,
			free_bytes:  0,
			depth:       0,
			stack:       nil,
		}
		sa.alloc_objs = int(binary.BigEndian.Uint32(data[offset:]))
		offset += 4
		sa.alloc_bytes = int(binary.BigEndian.Uint32(data[offset:]))
		offset += 4
		sa.free_objs = int(binary.BigEndian.Uint32(data[offset:]))
		offset += 4
		sa.free_bytes = int(binary.BigEndian.Uint32(data[offset:]))
		offset += 4
		sa.depth = int(binary.BigEndian.Uint32(data[offset:]))
		offset += 4
		sa.stack = make([]uint64, sa.depth)
		for i := 0; i < sa.depth; i++ {
			sa.stack[i] = binary.BigEndian.Uint64(data[offset:])
			offset += 8
		}
		prof.samples = append(prof.samples, sa)
	}
	return prof, nil
}

func showText(prof *Profile, inuse bool) {
	type node struct {
		Id          uint64
		alloc_objs  int
		alloc_bytes int
	}
	leafNodes := make(map[uint64]*node)
	totalBytes := 0
	if inuse {
		for _, sa := range prof.samples {
			if sa.alloc_bytes != sa.free_bytes {
				Id := sa.stack[0]
				if _, ok := leafNodes[Id]; !ok {
					leafNodes[Id] = &node{
						Id:          Id,
						alloc_objs:  0,
						alloc_bytes: 0,
					}
				}
				leafNodes[Id].alloc_objs += (sa.alloc_objs - sa.free_objs)
				inuseBytes := sa.alloc_bytes - sa.free_bytes
				leafNodes[Id].alloc_bytes += inuseBytes
				totalBytes += inuseBytes
			}
		}
	} else {
		for _, sa := range prof.samples {
			Id := sa.stack[0]
			if _, ok := leafNodes[Id]; !ok {
				leafNodes[Id] = &node{
					Id:          Id,
					alloc_objs:  0,
					alloc_bytes: 0,
				}
			}
			leafNodes[Id].alloc_bytes += sa.alloc_bytes
			leafNodes[Id].alloc_objs += sa.alloc_objs
			totalBytes += sa.alloc_bytes
		}
	}

	pairs := make([]*node, 0)
	for Id, lNode := range leafNodes {
		pairs = append(pairs, &node{
			Id:          Id,
			alloc_objs:  lNode.alloc_objs,
			alloc_bytes: lNode.alloc_bytes,
		})
	}
	sort.Slice(pairs, func(i, j int) bool {
		if pairs[i].alloc_bytes == pairs[j].alloc_bytes {
			return pairs[i].alloc_objs > pairs[j].alloc_objs
		}
		return pairs[i].alloc_bytes > pairs[j].alloc_bytes
	})

	if inuse {
		fmt.Println("-----------inuse space ranking----------")
	} else {
		fmt.Println("-----------alloc space ranking----------")
	}
	for i, pa := range pairs {
		s := fmt.Sprintf("%vth\t%v(kb)\t%v%%\t%v\t%v\n", i+1, float64(pa.alloc_bytes)/1024.0, float64(pa.alloc_bytes)*100/float64(totalBytes), pa.alloc_objs, prof.funcId2Name[pa.Id])
		if i < 5 && pa.alloc_bytes > 0 {
			s = KRED + s + KNRM
		}
		fmt.Print(s)
	}
}

func newDot(prof *Profile, inuse bool) string {
	flatNodes := make(map[uint64]int)
	cumNodes := make(map[uint64]int)
	totalBytes := 0
	for _, sa := range prof.samples {
		records := make(map[uint64]bool)
		for _, Id := range sa.stack {
			if !records[Id] {
				records[Id] = true
				if inuse {
					cumNodes[Id] += (sa.alloc_bytes - sa.free_bytes)
				} else {
					cumNodes[Id] += sa.alloc_bytes
				}
			}
		}
		leafId := sa.stack[0]
		if inuse {
			inuseBytes := sa.alloc_bytes - sa.free_bytes
			flatNodes[leafId] += inuseBytes
			totalBytes += inuseBytes
		} else {
			flatNodes[leafId] += sa.alloc_bytes
			totalBytes += sa.alloc_bytes
		}
	}

	type pair struct {
		Id    uint64
		count int
	}
	pairs := make([]*pair, 0)
	for Id, count := range flatNodes {
		pairs = append(pairs, &pair{
			Id:    Id,
			count: count,
		})
	}
	sort.Slice(pairs, func(i, j int) bool {
		return pairs[i].count > pairs[j].count
	})
	ranking := make(map[uint64]int)
	for i, pa := range pairs {
		ranking[pa.Id] = i + 1
	}

	type vector struct {
		src uint64
		dst uint64
	}
	vectors := make(map[vector]int)
	parents := make(map[uint64]bool)
	for _, sa := range prof.samples {
		records := make(map[vector]bool)
		for i := sa.depth - 1; i > 0; i-- {
			vec := vector{
				src: sa.stack[i],
				dst: sa.stack[i-1],
			}
			if !records[vec] {
				records[vec] = true
				if inuse {
					vectors[vec] += (sa.alloc_bytes - sa.free_bytes)
				} else {
					vectors[vec] += sa.alloc_bytes
				}
			}
			parents[vec.src] = true
		}
	}

	fixFuncName := func(name string) string {
		return strings.Replace(name, "\"", "'", -1)
	}
	var dot strings.Builder
	dot.WriteString("digraph G {\n")
	for Id, count := range cumNodes {
		dot.WriteString(fmt.Sprintf("\tnode%v [label=\"%v\\r%v (%v%%)\\r",
			Id, fixFuncName(prof.funcId2Name[Id]), flatNodes[Id], flatNodes[Id]*100/totalBytes))
		if parents[Id] {
			dot.WriteString(fmt.Sprintf("%v (%v%%)\\r", count, count*100/totalBytes))
		}
		dot.WriteString("\";")

		fontsize := flatNodes[Id] * 100 / totalBytes
		if fontsize < 10 {
			fontsize = 10
		}
		dot.WriteString(fmt.Sprintf("fontsize=%v;", fontsize))
		dot.WriteString("shape=box;")
		if ranking[Id] > 0 && ranking[Id] <= 5 {
			dot.WriteString("color=red;")
		}
		dot.WriteString("];\n")
	}
	for vec, count := range vectors {
		linewidth := float64(count) * 8.0 / float64(totalBytes)
		if linewidth < 0.2 {
			linewidth = 0.2
		}
		dot.WriteString(fmt.Sprintf("\tnode%v->node%v [style=\"setlinewidth(%v)\" label=%v];\n", vec.src, vec.dst, linewidth, count))
	}
	dot.WriteString("}\n")

	return dot.String()
}

func showPic(prof *Profile, inuse bool, png, svg string) {
	dot := newDot(prof, inuse)
	graph, err := graphviz.ParseBytes([]byte(dot))
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	if png != "" {
		g := graphviz.New()
		err = g.RenderFilename(graph, graphviz.PNG, png)
		if err != nil {
			fmt.Println(err)
			os.Exit(1)
		}
	}
	if svg != "" {
		g := graphviz.New()
		err = g.RenderFilename(graph, graphviz.SVG, svg)
		if err != nil {
			fmt.Println(err)
			os.Exit(1)
		}
	}
}

func showInfo(prof *Profile, outFile string) {
	type pair struct {
		Id   uint64
		name string
	}
	pairs := make([]*pair, 0)
	for Id, name := range prof.funcId2Name {
		pairs = append(pairs, &pair{Id: Id, name: name})
	}
	sort.Slice(pairs, func(i, j int) bool {
		return pairs[i].Id < pairs[j].Id
	})
	var builder strings.Builder
	builder.WriteString("FuncId\t:\tFuncName\n")
	for _, pa := range pairs {
		builder.WriteString(fmt.Sprintf("%d\t:\t%s\n", pa.Id, pa.name))
	}
	builder.WriteString("--------------------------------------------------------------------\n")

	sort.Slice(prof.samples, func(i, j int) bool {
		return (prof.samples[i].alloc_bytes - prof.samples[i].free_bytes) > (prof.samples[j].alloc_bytes - prof.samples[j].free_bytes)
	})
	builder.WriteString("AllocBytes\tFreeBytes\tAllocObjs\tFreeObjs\t:\tBacktrace\n")
	for _, sa := range prof.samples {
		builder.WriteString(fmt.Sprintf("%d\t%d\t%d\t%d\t:\t", sa.alloc_bytes, sa.free_bytes, sa.alloc_objs, sa.free_objs))
		for i := sa.depth - 1; i >= 0; i-- {
			if i == 0 {
				builder.WriteString(fmt.Sprintf("%d\n", sa.stack[i]))
			} else {
				builder.WriteString(fmt.Sprintf("%d -> ", sa.stack[i]))
			}
		}
	}

	if err := ioutil.WriteFile(outFile, []byte(builder.String()), 0666); err != nil {
		fmt.Println(err)
	}
}

func main() {
	flag.StringVar(&input, "i", "", "input file")
	flag.BoolVar(&text, "text", false, "show text sort data")
	flag.BoolVar(&inuse, "inuse", false, "show inuse space data")
	flag.StringVar(&png, "png", "", "generate png file")
	flag.StringVar(&svg, "svg", "", "generate svg file")
	flag.StringVar(&info, "info", "", "dump mem profile data detail info to file")
	flag.Parse()

	if input == "" {
		flag.Usage()
		os.Exit(1)
	}
	prof, err := parseFile(input)
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	if text {
		showText(prof, inuse)
	}
	if png != "" || svg != "" {
		showPic(prof, inuse, png, svg)
	}
	if info != "" {
		showInfo(prof, info)
	}
}
