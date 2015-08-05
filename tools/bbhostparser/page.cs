using System.Collections.Generic;

namespace bbhostparser
{
    public class page
    {
        public page()
        {
            groups = new List<group>();
            subpages = new List<subpage>();
        }

        public string id { get; set; }
        public int row_id { get; set; }
        public string xymon_id { get; set; }
        public string name { get; set; }
        public IList<group> groups { get; set; }
        public IList<subpage> subpages { get; set; }
    }

    public class subpage
    {
        public subpage()
        {
            groups = new List<group>();
        }

        public string id { get; set; }
        public int row_id { get; set; }
        public string xymon_id { get; set; }
        public string name { get; set; }
        public IList<group> groups { get; set; }
    }

    public class group
    {
        public group()
        {
            nodes = new List<node>();
        }
        public string id { get; set; }
        public int row_id { get; set; }
        public string group_type { get; set; }
        public string name { get; set; }
        public string xymon_settings { get; set; }
        public IList<node> nodes { get; set; }
    }

    public class node
    {
        public string id { get; set; }
        public int row_id { get; set; }
        public string ip_number { get; set; }
        public string name { get; set; }
        public string xymon_settings { get; set; }
    }
}
