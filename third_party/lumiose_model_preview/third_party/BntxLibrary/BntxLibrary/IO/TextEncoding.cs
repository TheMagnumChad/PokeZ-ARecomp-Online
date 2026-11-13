using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;

namespace BntxLibrary.Util
{
    public static class TextEncoding
    {
        static bool Registered = false;

        public static Encoding ShiftJIS()
        {
            TryInit();
            return Encoding.GetEncoding("shift_jis");
        }

        public static void TryInit()
        {
            if (!Registered)
            {
                Registered = true;
                Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
            }
        }
    }
}
