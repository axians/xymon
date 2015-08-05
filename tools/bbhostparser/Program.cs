using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using Newtonsoft.Json;

namespace bbhostparser
{
    internal class Program
    {
        private static void Main(string[] args)
        {
            foreach (var file in Directory.GetFiles(Directory.GetCurrentDirectory(), "*.txt"))
            {
                if (file.Contains("generated_")) continue;
                var fi = new FileInfo(file);
                var json_file_name = fi.Name.Replace(fi.Extension, ".json");
                var generated_bbhosts_file_name = string.Format("generated_{0}", fi.Name);
                Console.WriteLine("Converting {0} to {1}...", fi.Name, json_file_name);
                ConvertBBHostToJson(fi.Name, json_file_name);
                Console.WriteLine("Converting {0} to {1}...", json_file_name, fi.Name);
                ConvertJsonToBBhost(json_file_name, generated_bbhosts_file_name);
                Console.WriteLine();
            }
            Console.WriteLine("Done");
            Console.ReadLine();
        }

        private static void ConvertJsonToBBhost(string input_file_name, string output_file_name)
        {
            var list_of_pages = JsonConvert.DeserializeObject<List<page>>(File.ReadAllText(input_file_name, Encoding.UTF8));
            var sb = new StringBuilder();

            foreach (var p in list_of_pages)
            {
                sb.AppendLine(string.Format("page\t{0}\t{1}", p.xymon_id, p.name));

                foreach (var g in p.groups)
                {
                    sb.AppendLine(string.Format("{0}\t{1}\t{2}", g.group_type, g.xymon_settings, g.name));

                    foreach (var n in g.nodes)
                    {
                        sb.AppendLine(string.Format("{0}\t{1}\t\t# {2}", n.ip_number, n.name, n.xymon_settings));
                    }
                    sb.AppendLine();
                }

                foreach (var sp in p.subpages)
                {
                    sb.AppendLine(string.Format("subpage\t{0}\t{1}", sp.xymon_id, sp.name));
                    foreach (var g in sp.groups)
                    {
                        sb.AppendLine(string.Format("{0}\t{1}\t{2}", g.group_type, g.xymon_settings, g.name));
                        
                        foreach (var n in g.nodes)
                        {
                            sb.AppendLine(string.Format("{0}\t{1}\t\t# {2}", n.ip_number, n.name, n.xymon_settings));
                        }
                        sb.AppendLine();
                    }
                }

                sb.AppendLine();
            }

            File.WriteAllText(output_file_name, sb.ToString(), Encoding.GetEncoding("iso-8859-1"));
        }

        private static void ConvertBBHostToJson(string input_file_name, string output_file_name)
        {
            var list_of_pages = new List<page>();

            var bbking = File.ReadLines(input_file_name, Encoding.GetEncoding("iso-8859-1")).ToList();

            var page_possitions = FindAllThethings(bbking, "page");
            var subpage_possitions = FindAllThethings(bbking, "subpage");
            var group_compress_possitions = FindAllThethings(bbking, "group-compress");
            var group_except_possitions = FindAllThethings(bbking, "group-except");
            var node_possitions = FindAllTheNodes(bbking);

            foreach (var p in page_possitions)
            {
                list_of_pages.Add(GetPageByRowNumber(bbking, p));
            }

            foreach (var sp in subpage_possitions)
            {
                var parent_pos = FindParent(page_possitions, sp);
                var subp = GetSubPageByRowNumber(bbking, sp);

                list_of_pages.Single(x => x.row_id == parent_pos).subpages.Add(subp);
            }

            foreach (var g in group_compress_possitions)
            {

                var parent_pos = (FindParent(page_possitions.Union(subpage_possitions), g));
                var gr = GetGroupCompressByRowNumber(bbking, g);

                var p = list_of_pages.SingleOrDefault(x => x.row_id == parent_pos);
                if (p != null)
                {
                    p.groups.Add(gr);
                }
                else
                {
                    p = list_of_pages.FirstOrDefault(y => y.subpages.FirstOrDefault(z => z.row_id == parent_pos) != null);
                    p.subpages.First(x => x.row_id == parent_pos).groups.Add(gr);
                }
            }

            foreach (var g in group_except_possitions)
            {
                var parent_pos = (FindParent(page_possitions.Union(subpage_possitions), g));
                var gr = GetGroupExceptByRowNumber(bbking, g);

                var p = list_of_pages.SingleOrDefault(x => x.row_id == parent_pos);
                if (p != null)
                {
                    p.groups.Add(gr);
                }
                else
                {
                    p = list_of_pages.FirstOrDefault(y => y.subpages.FirstOrDefault(z => z.row_id == parent_pos) != null);
                    p.subpages.First(x => x.row_id == parent_pos).groups.Add(gr);
                }
            }

            foreach (var possition in node_possitions)
            {
                var parent_pos = (FindParent(group_compress_possitions.Union(group_except_possitions), possition));
                var n = GetNodeByRowNumber(bbking, possition);

                var z = list_of_pages
                    .SelectMany(p => p.subpages)
                    .SelectMany(s => s.groups)
                    .Where(g => g.row_id == parent_pos)
                    .ToList();

                if (z.Count > 0)
                {
                    z.First().nodes.Add(n);
                }
                else
                {

                    z = list_of_pages
                        .SelectMany(p => p.groups)
                        .Where(g => g.row_id == parent_pos)
                        .ToList();

                    if (z.Count > 0)
                    {
                        z.First().nodes.Add(n);
                    }

                }
            }

            File.WriteAllText(output_file_name, JsonConvert.SerializeObject(list_of_pages, Formatting.Indented), Encoding.UTF8);
        }

        private static int FindParent(IEnumerable<int> page_pos, int child_row_id)
        {
            return page_pos.OrderBy(x => x).Last(x => x < child_row_id);
        }

        private static IList<int> FindAllThethings(IList<string> lines, string line_type)
        {
            var pos_list = new List<int>();

            for (var i = 0; i < lines.Count; i++)
            {
                if(lines[i].StartsWith(line_type))
                    pos_list.Add(i);
            }

            return pos_list;
        }

        private static IEnumerable<int> FindAllTheNodes(IList<string> lines)
        {
            var pos_list = new List<int>();

            for (var i = 0; i < lines.Count; i++)
            {
                if (!lines[i].StartsWith("page") &&
                    !lines[i].StartsWith("subpage") &&
                    !lines[i].StartsWith("group-except") &&
                    !lines[i].StartsWith("group-compress") &&
                    !lines[i].StartsWith("#") &&
                    !lines[i].StartsWith("sts") &&
                    !lines[i].StartsWith("include") &&
                    lines[i].Trim().Length > 0)
                { 
                    pos_list.Add(i);
                }
            }

            return pos_list;
        }

        private static page GetPageByRowNumber(IList<string> lines, int row_number)
        {
            var line = lines[row_number];
            var line_components = line.Split(" ".ToCharArray());

            return new page
            {
                id = Guid.NewGuid().ToString(),
                row_id = row_number,
                xymon_id = line_components[1],
                name = FlattenStringArray(line_components, 2).Replace("page", string.Empty)
            };
        }

        private static subpage GetSubPageByRowNumber(IList<string> lines, int row_number)
        {
            var line = lines[row_number];
            var line_components = line.Split(" ".ToCharArray());

            return new subpage()
            {
                id = Guid.NewGuid().ToString(),
                row_id = row_number,
                xymon_id = line_components[1],
                name = FlattenStringArray(line_components, 2).Replace("page", string.Empty)
            };
        }

        private static group GetGroupCompressByRowNumber(IList<string> lines, int row_number)
        {
            var line = lines[row_number];
            var line_components = line.Split(" ".ToCharArray());

            if(line_components.Length == 1)
                line_components = line.Split("\t".ToCharArray());

            return new group()
            {
                id = Guid.NewGuid().ToString(),
                row_id = row_number,
                name = FlattenStringArray(line_components, 1),
                group_type = "group-compress"
            };
        }

        private static group GetGroupExceptByRowNumber(IList<string> lines, int row_number)
        {
            var line = lines[row_number];
            var line_components = line.Split(" ".ToCharArray());

            if (line_components.Length == 1)
                line_components = line.Split("\t".ToCharArray());

            return new group()
            {
                id = Guid.NewGuid().ToString(),
                row_id = row_number,
                name = FlattenStringArray(line_components, 2),
                group_type = "group-except",
                xymon_settings = line_components[1]
            };
        }

        private static node GetNodeByRowNumber(IList<string> lines, int row_number)
        {

            var line = lines[row_number];
            var regex = new Regex(@"[ ]{2,}", RegexOptions.Compiled);
            line = regex.Replace(line, @" ");

            line = line.Replace("\t", " ");
            var line_components = line.Split(" ".ToCharArray());

            var n = new node
            {
                id = Guid.NewGuid().ToString(),
                row_id = row_number,
                ip_number = line_components[0],
                name = line_components[1],
                xymon_settings = FlattenStringArray(line_components, 2).Replace("#", string.Empty).Trim()
            };
            return n;
        }

        private static string FlattenStringArray(string[] array_of_strings, int start_from = 0)
        {
            var sb = new StringBuilder();
            
            for (var i = start_from; i < array_of_strings.Length; i++)
            {
                sb.Append(array_of_strings[i] + " ");
            }

            return sb.ToString().TrimEnd();
        }
    }
}
